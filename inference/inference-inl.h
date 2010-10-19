/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation, 
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file   inference-inl.h
 * @brief  inference template definitions
 * @author Frank Dellaert, Richard Roberts
 */

#pragma once

#include <limits>
#include <map>
#include <stdexcept>

#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/pool/pool_alloc.hpp>

#include <ccolamd.h>

#include <gtsam/base/timing.h>
#include <gtsam/inference/inference.h>
#include <gtsam/inference/FactorGraph-inl.h>
#include <gtsam/inference/BayesNet-inl.h>
#include <gtsam/inference/Conditional.h>

using namespace std;

namespace gtsam {

/* ************************************************************************* */
template<class FactorGraph>
inline typename FactorGraph::bayesnet_type::shared_ptr Inference::Eliminate(const FactorGraph& factorGraph) {

  // Create a copy of the factor graph to eliminate in-place
  FactorGraph eliminationGraph(factorGraph);
  typename FactorGraph::variableindex_type variableIndex(eliminationGraph);

  return Eliminate(eliminationGraph, variableIndex);
}

/* ************************************************************************* */
//template<class FACTOR>
//BayesNet<Conditional>::shared_ptr Inference::EliminateSymbolic(const FactorGraph<FACTOR>& factorGraph) {
//
//  // Create a copy of the factor graph to eliminate in-place
//  FactorGraph<gtsam::Factor> eliminationGraph(factorGraph);
//  VariableIndex<> variableIndex(eliminationGraph);
//
//  typename BayesNet<Conditional>::shared_ptr bayesnet(new BayesNet<Conditional>());
//
//  // Eliminate variables one-by-one, updating the eliminated factor graph and
//  // the variable index.
//  for(Index var = 0; var < variableIndex.size(); ++var) {
//    Conditional::shared_ptr conditional(EliminateOneSymbolic(eliminationGraph, variableIndex, var));
//    if(conditional) // Will be NULL if the variable did not appear in the factor graph.
//      bayesnet->push_back(conditional);
//  }
//
//  return bayesnet;
//}

/* ************************************************************************* */
template<class FactorGraph>
inline typename FactorGraph::bayesnet_type::shared_ptr
Inference::Eliminate(FactorGraph& factorGraph, typename FactorGraph::variableindex_type& variableIndex) {

  return EliminateUntil(factorGraph, variableIndex.size(), variableIndex);
}

/* ************************************************************************* */
template<class FactorGraph>
inline typename FactorGraph::bayesnet_type::shared_ptr
Inference::EliminateUntil(const FactorGraph& factorGraph, Index bound) {

  // Create a copy of the factor graph to eliminate in-place
  FactorGraph eliminationGraph(factorGraph);
  typename FactorGraph::variableindex_type variableIndex(eliminationGraph);

  return EliminateUntil(eliminationGraph, bound, variableIndex);
}

/* ************************************************************************* */
template<class FactorGraph>
typename FactorGraph::bayesnet_type::shared_ptr
Inference::EliminateUntil(FactorGraph& factorGraph, Index bound, typename FactorGraph::variableindex_type& variableIndex) {

  typename FactorGraph::bayesnet_type::shared_ptr bayesnet(new typename FactorGraph::bayesnet_type);

  // Eliminate variables one-by-one, updating the eliminated factor graph and
  // the variable index.
  for(Index var = 0; var < bound; ++var) {
    typename FactorGraph::bayesnet_type::sharedConditional conditional(EliminateOne(factorGraph, variableIndex, var));
    if(conditional) // Will be NULL if the variable did not appear in the factor graph.
      bayesnet->push_back(conditional);
  }

  return bayesnet;
}

/* ************************************************************************* */
template<class FactorGraph>
typename FactorGraph::bayesnet_type::sharedConditional
Inference::EliminateOne(FactorGraph& factorGraph, typename FactorGraph::variableindex_type& variableIndex, Index var) {

  /* This function performs symbolic elimination of a variable, comprising
   * combining involved factors (analogous to "assembly" in SPQR) followed by
   * eliminating to an upper-trapezoidal factor using spqr_front.  This
   * function performs the bookkeeping necessary for high performance.
   *
   * When combining factors, variables are merge sorted so that they remain
   * in elimination order in the combined factor.  GaussianFactor combines
   * rows such that the row index after the last structural non-zero in each
   * column increases monotonically (referred to as the "staircase" pattern in
   * SPQR).  The variable ordering is passed into the factor's Combine(...)
   * function, which does the work of actually building the combined factor
   * (for a GaussianFactor this assembles the augmented matrix).
   *
   * Next, this function calls the factor's eliminateFirst() function, which
   * factorizes the factor into a conditional on the first variable and a
   * factor on the remaining variables.  In addition, this function updates the
   * bookkeeping of the pattern of structural non-zeros.  The GaussianFactor
   * calls spqr_front during eliminateFirst(), which reduces its matrix to
   * upper-trapezoidal form.
   *
   * Returns NULL if the variable does not appear in factorGraph.
   */

  tic("EliminateOne");

  // Get the factors involving the eliminated variable
  typename FactorGraph::variableindex_type::mapped_type& varIndexEntry(variableIndex[var]);
  typedef typename FactorGraph::variableindex_type::mapped_factor_type mapped_factor_type;

  if(!varIndexEntry.empty()) {

    vector<size_t> removedFactors(varIndexEntry.size());
    transform(varIndexEntry.begin(), varIndexEntry.end(), removedFactors.begin(),
        boost::lambda::bind(&FactorGraph::variableindex_type::mapped_factor_type::factorIndex, boost::lambda::_1));

    // The new joint factor will be the last one in the factor graph
    size_t jointFactorIndex = factorGraph.size();

    static const bool debug = false;

    if(debug) {
      cout << "Eliminating " << var;
      factorGraph.print(" from graph: ");
      cout << removedFactors.size() << " factors to remove" << endl;
    }

    // Compute the involved keys, uses the variableIndex to mark whether each
    // key has been added yet, but the positions stored in the variableIndex are
    // from the unsorted positions and will be fixed later.
    tic("EliminateOne: Find involved vars");
    map<Index, size_t, std::less<Index>, boost::fast_pool_allocator<pair<const Index,size_t> > > involvedKeys; // Variable and original order as discovered
    BOOST_FOREACH(size_t removedFactorI, removedFactors) {
      if(debug) cout << removedFactorI << " is involved" << endl;
      // If the factor has not previously been removed
      if(removedFactorI < factorGraph.size() && factorGraph[removedFactorI]) {
        // Loop over the variables involved in the removed factor to update the
        // variable index and joint factor positions of each variable.
        BOOST_FOREACH(Index involvedVariable, factorGraph[removedFactorI]->keys()) {
          // Mark the new joint factor as involving each variable in the removed factor.
          assert(!variableIndex[involvedVariable].empty());
          if(variableIndex[involvedVariable].back().factorIndex != jointFactorIndex) {
            if(debug) cout << "  pulls in variable " << involvedVariable << endl;
            size_t varpos = involvedKeys.size();
            variableIndex[involvedVariable].push_back(mapped_factor_type(jointFactorIndex, varpos));
#ifndef NDEBUG
            bool inserted =
#endif
                involvedKeys.insert(make_pair(involvedVariable, varpos)).second;
            assert(inserted);
          } else if(debug)
            cout << "  involves variable " << involvedVariable << " which was previously discovered" << endl;
        }
      }
    }
    toc("EliminateOne: Find involved vars");
    if(debug) cout << removedFactors.size() << " factors to remove" << endl;

    // Compute the permutation to go from the original varpos to the sorted
    // joint factor varpos
    if(debug) cout << "Sorted keys:";
    tic("EliminateOne: Sort involved vars");
    vector<size_t> varposPermutation(involvedKeys.size(), numeric_limits<size_t>::max());
    vector<Index> sortedKeys(involvedKeys.size());
    {
      size_t sortedVarpos = 0;
      const map<Index, size_t, std::less<Index>, boost::fast_pool_allocator<pair<const Index,size_t> > >& involvedKeysC(involvedKeys);
      for(map<Index, size_t, std::less<Index>, boost::fast_pool_allocator<pair<const Index,size_t> > >::const_iterator key_pos=involvedKeysC.begin(); key_pos!=involvedKeysC.end(); ++key_pos) {
        sortedKeys[sortedVarpos] = key_pos->first;
        assert(varposPermutation[key_pos->second] == numeric_limits<size_t>::max());
        varposPermutation[key_pos->second] = sortedVarpos;
        if(debug) cout << " " << key_pos->first << " (" << key_pos->second << "->" << sortedVarpos << ")  ";
        ++ sortedVarpos;
      }
    }
    toc("EliminateOne: Sort involved vars");
    if(debug) cout << endl;

    assert(sortedKeys.front() == var);
    if(debug) cout << removedFactors.size() << " factors to remove" << endl;

    // Fix the variable positions in the variableIndex
    tic("EliminateOne: Fix varIndex");
    for(size_t sortedPos=0; sortedPos<sortedKeys.size(); ++sortedPos) {
      Index var = sortedKeys[sortedPos];
      assert(!variableIndex[var].empty());
      assert(variableIndex[var].back().factorIndex == jointFactorIndex);
      assert(sortedPos == varposPermutation[variableIndex[var].back().variablePosition]);
      if(debug) cout << "Fixing " << var << "  " << variableIndex[var].back().variablePosition << "->" << sortedPos << endl;
      variableIndex[var].back().variablePosition = sortedPos;
    }
    toc("EliminateOne: Fix varIndex");

    // Fill in the jointFactorPositions
    tic("EliminateOne: Fill jointFactorPositions");
    vector<size_t> removedFactorIdxs;
    removedFactorIdxs.reserve(removedFactors.size());
    vector<vector<size_t> > jointFactorPositions;
    jointFactorPositions.reserve(removedFactors.size());
    if(debug) cout << removedFactors.size() << " factors to remove" << endl;
    BOOST_FOREACH(size_t removedFactorI, removedFactors) {
      if(debug) cout << "Fixing variable positions for factor " << removedFactorI << endl;
      // If the factor has not previously been removed
      if(removedFactorI < factorGraph.size() && factorGraph[removedFactorI]) {

        // Allocate space
        jointFactorPositions.push_back(vector<size_t>());
        vector<size_t>& jointFactorPositionsCur(jointFactorPositions.back());
        jointFactorPositionsCur.reserve(factorGraph[removedFactorI]->keys().size());
        removedFactorIdxs.push_back(removedFactorI);

        // Loop over the variables involved in the removed factor to update the
        // variable index and joint factor positions of each variable.
        BOOST_FOREACH(Index involvedVariable, factorGraph[removedFactorI]->keys()) {
          // Mark the new joint factor as involving each variable in the removed factor
          assert(!variableIndex[involvedVariable].empty());
          assert(variableIndex[involvedVariable].back().factorIndex == jointFactorIndex);
          const size_t varpos = variableIndex[involvedVariable].back().variablePosition;
          jointFactorPositionsCur.push_back(varpos);
          if(debug) cout << "Variable " << involvedVariable << " from factor " << removedFactorI;
          if(debug) cout << " goes in position " << varpos << " of the joint factor" << endl;
          assert(sortedKeys[varpos] == involvedVariable);
        }
      }
    }
    toc("EliminateOne: Fill jointFactorPositions");

    // Join the factors and eliminate the variable from the joint factor
    tic("EliminateOne: Combine");
    typename FactorGraph::sharedFactor jointFactor(
        FactorGraph::Factor::Combine(
            factorGraph, variableIndex, removedFactorIdxs, sortedKeys, jointFactorPositions));
    toc("EliminateOne: Combine");

    // Remove the original factors
    BOOST_FOREACH(size_t removedFactorI, removedFactors) {
      if(removedFactorI < factorGraph.size() && factorGraph[removedFactorI])
        factorGraph.remove(removedFactorI);
    }

    typename FactorGraph::bayesnet_type::sharedConditional conditional;
    tic("EliminateOne: eliminateFirst");
    conditional = jointFactor->eliminateFirst();   // Eliminate the first variable in-place
    toc("EliminateOne: eliminateFirst");
    tic("EliminateOne: store eliminated");
    variableIndex[sortedKeys.front()].pop_back();  // Unmark the joint factor from involving the eliminated variable
    factorGraph.push_back(jointFactor);  // Put the eliminated factor into the factor graph
    toc("EliminateOne: store eliminated");

    toc("EliminateOne");

    return conditional;

  } else { // varIndexEntry.empty()
    toc("EliminateOne");
    return typename FactorGraph::bayesnet_type::sharedConditional();
  }
}

/* ************************************************************************* */
template<class FactorGraph, class VarContainer>
FactorGraph Inference::Marginal(const FactorGraph& factorGraph, const VarContainer& variables) {

  // Compute a COLAMD permutation with the marginal variables constrained to the end
  typename FactorGraph::variableindex_type varIndex(factorGraph);
  Permutation::shared_ptr permutation(Inference::PermutationCOLAMD(varIndex, variables));
  Permutation::shared_ptr permutationInverse(permutation->inverse());

  // Copy and permute the factors
  varIndex.permute(*permutation);
  FactorGraph eliminationGraph; eliminationGraph.reserve(factorGraph.size());
  BOOST_FOREACH(const typename FactorGraph::sharedFactor& factor, factorGraph) {
    typename FactorGraph::sharedFactor permFactor(new typename FactorGraph::Factor(*factor));
    permFactor->permuteWithInverse(*permutationInverse);
    eliminationGraph.push_back(permFactor);
  }

  // Eliminate all variables
  typename FactorGraph::bayesnet_type::shared_ptr bn(Inference::Eliminate(eliminationGraph, varIndex));

  // The last conditionals in the eliminated BayesNet contain the marginal for
  // the variables we want.  Undo the permutation as we add the marginal
  // factors.
  FactorGraph marginal; marginal.reserve(variables.size());
  typename FactorGraph::bayesnet_type::const_reverse_iterator conditional = bn->rbegin();
  for(Index j=0; j<variables.size(); ++j, ++conditional) {
    typename FactorGraph::sharedFactor factor(new typename FactorGraph::Factor(**conditional));
    factor->permuteWithInverse(*permutation);
    marginal.push_back(factor);
    assert(std::find(variables.begin(), variables.end(), (*permutation)[(*conditional)->key()]) != variables.end());
  }

  // Undo the permutation
  return marginal;
}

/* ************************************************************************* */
template<class VariableIndexType, typename ConstraintContainer>
Permutation::shared_ptr Inference::PermutationCOLAMD(const VariableIndexType& variableIndex, const ConstraintContainer& constrainLast) {
  size_t nEntries = variableIndex.nEntries(), nFactors = variableIndex.nFactors(), nVars = variableIndex.size();
  // Convert to compressed column major format colamd wants it in (== MATLAB format!)
  int Alen = ccolamd_recommended(nEntries, nFactors, nVars); /* colamd arg 3: size of the array A */
  int * A = new int[Alen]; /* colamd arg 4: row indices of A, of size Alen */
  int * p = new int[nVars + 1]; /* colamd arg 5: column pointers of A, of size n_col+1 */
  int * cmember = new int[nVars]; /* Constraint set of A, of size n_col */

  static const bool debug = false;

  p[0] = 0;
  int count = 0;
  for(Index var = 0; var < variableIndex.size(); ++var) {
    const typename VariableIndexType::mapped_type& column(variableIndex[var]);
    size_t lastFactorId = numeric_limits<size_t>::max();
    BOOST_FOREACH(const typename VariableIndexType::mapped_factor_type& factor_pos, column) {
      if(lastFactorId != numeric_limits<size_t>::max())
        assert(factor_pos.factorIndex > lastFactorId);
      A[count++] = factor_pos.factorIndex; // copy sparse column
      if(debug) cout << "A[" << count-1 << "] = " << factor_pos.factorIndex << endl;
    }
    p[var+1] = count; // column j (base 1) goes from A[j-1] to A[j]-1
    cmember[var] = 0;
  }

  // If at least some variables are not constrained to be last, constrain the
  // ones that should be constrained.
  if(constrainLast.size() < variableIndex.size()) {
    BOOST_FOREACH(Index var, constrainLast) {
      assert(var < nVars);
      cmember[var] = 1;
    }
  }

  assert((size_t)count == variableIndex.nEntries());

  if(debug)
    for(size_t i=0; i<nVars+1; ++i)
      cout << "p[" << i << "] = " << p[i] << endl;

  double* knobs = NULL; /* colamd arg 6: parameters (uses defaults if NULL) */
  int stats[CCOLAMD_STATS]; /* colamd arg 7: colamd output statistics and error codes */

  // call colamd, result will be in p
  /* returns (1) if successful, (0) otherwise*/
  int rv = ccolamd(nFactors, nVars, Alen, A, p, knobs, stats, cmember);
  if(rv != 1)
    throw runtime_error((boost::format("ccolamd failed with return value %1%")%rv).str());
  delete[] A; // delete symbolic A
  delete[] cmember;

  // Convert elimination ordering in p to an ordering
  Permutation::shared_ptr permutation(new Permutation(nVars));
  for (Index j = 0; j < nVars; j++) {
    permutation->operator[](j) = p[j];
    if(debug) cout << "COLAMD:  " << j << "->" << p[j] << endl;
  }
  if(debug) cout << "COLAMD:  p[" << nVars << "] = " << p[nVars] << endl;
  delete[] p; // delete colamd result vector

  return permutation;
}


//	/* ************************************************************************* */
//	/* eliminate one node from the factor graph                           */
//	/* ************************************************************************* */
//	template<class Factor,class Conditional>
//	boost::shared_ptr<Conditional> eliminateOne(FactorGraph<Factor>& graph, Index key) {
//
//		// combine the factors of all nodes connected to the variable to be eliminated
//		// if no factors are connected to key, returns an empty factor
//		boost::shared_ptr<Factor> joint_factor = removeAndCombineFactors(graph,key);
//
//		// eliminate that joint factor
//		boost::shared_ptr<Factor> factor;
//		boost::shared_ptr<Conditional> conditional;
//		boost::tie(conditional, factor) = joint_factor->eliminate(key);
//
//		// add new factor on separator back into the graph
//		if (!factor->empty()) graph.push_back(factor);
//
//		// return the conditional Gaussian
//		return conditional;
//	}
//
//	/* ************************************************************************* */
//	// This doubly templated function is generic. There is a GaussianFactorGraph
//	// version that returns a more specific GaussianBayesNet.
//	// Note, you will need to include this file to instantiate the function.
//	/* ************************************************************************* */
//	template<class Factor,class Conditional>
//	BayesNet<Conditional> eliminate(FactorGraph<Factor>& factorGraph, const Ordering& ordering)
//	{
//		BayesNet<Conditional> bayesNet; // empty
//
//		BOOST_FOREACH(Index key, ordering) {
//			boost::shared_ptr<Conditional> cg = eliminateOne<Factor,Conditional>(factorGraph,key);
//			bayesNet.push_back(cg);
//		}
//
//		return bayesNet;
//	}

//	/* ************************************************************************* */
//	template<class Factor, class Conditional>
//	pair< BayesNet<Conditional>, FactorGraph<Factor> >
//	factor(const BayesNet<Conditional>& bn, const Ordering& keys) {
//		// Convert to factor graph
//		FactorGraph<Factor> factorGraph(bn);
//
//		// Get the keys of all variables and remove all keys we want the marginal for
//		Ordering ord = bn.ordering();
//		BOOST_FOREACH(Index key, keys) ord.remove(key); // TODO: O(n*k), faster possible?
//
//		// eliminate partially,
//		BayesNet<Conditional> conditional = eliminate<Factor,Conditional>(factorGraph,ord);
//
//		// at this moment, the factor graph only encodes P(keys)
//		return make_pair(conditional,factorGraph);
//		}
//
//	/* ************************************************************************* */
//	template<class Factor, class Conditional>
//	FactorGraph<Factor> marginalize(const BayesNet<Conditional>& bn, const Ordering& keys) {
//
//		// factor P(X,Y) as P(X|Y)P(Y), where Y corresponds to  keys
//		pair< BayesNet<Conditional>, FactorGraph<Factor> > factors =
//				gtsam::factor<Factor,Conditional>(bn,keys);
//
//		// throw away conditional, return marginal P(Y)
//		return factors.second;
//		}

	/* ************************************************************************* */
//	pair<Vector,Matrix> marginalGaussian(const GaussianFactorGraph& fg, const Symbol& key) {
//
//		// todo: this does not use colamd!
//
//		list<Symbol> ord;
//		BOOST_FOREACH(const Symbol& k, fg.keys()) {
//			if(k != key)
//				ord.push_back(k);
//		}
//		Ordering ordering(ord);
//
//		// Now make another factor graph where we eliminate all the other variables
//		GaussianFactorGraph marginal(fg);
//		marginal.eliminate(ordering);
//
//		GaussianFactor::shared_ptr factor;
//		for(size_t i=0; i<marginal.size(); i++)
//			if(marginal[i] != NULL) {
//				factor = marginal[i];
//				break;
//			}
//
//		if(factor->keys().size() != 1 || factor->keys().front() != key)
//			throw runtime_error("Didn't get the right marginal!");
//
//		VectorValues mean_cfg(marginal.optimize(Ordering(key)));
//		Matrix A(factor->get_A(key));
//
//		return make_pair(mean_cfg[key], inverse(prod(trans(A), A)));
//	}

	/* ************************************************************************* */

} // namespace gtsam
