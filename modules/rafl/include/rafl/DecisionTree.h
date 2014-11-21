/**
 * rafl: DecisionTree.h
 */

#ifndef H_RAFL_DECISIONTREE
#define H_RAFL_DECISIONTREE

#include <functional>
#include <set>
#include <stdexcept>

#include <tvgutil/PriorityQueue.h>

#include "decisionfunctions/DecisionFunctionGenerator.h"
#include "examples/ExampleReservoir.h"
#include "examples/ExampleUtil.h"

namespace rafl {

/**
 * \brief An instance of an instantiation of this class template represents a tree suitable for use within a random forest.
 */
template <typename Label>
class DecisionTree
{
  //#################### NESTED TYPES ####################
private:
  /**
   * \brief An instance of this struct represents a node in the tree.
   */
  struct Node
  {
    //~~~~~~~~~~~~~~~~~~~~ PUBLIC VARIABLES ~~~~~~~~~~~~~~~~~~~~

    /** The index of the node's left child in the tree's node array. */
    int m_leftChildIndex;

    /** The reservoir of examples currently stored in the node. */
    ExampleReservoir<Label> m_reservoir;

    /** The index of the node's right child in the tree's node array. */
    int m_rightChildIndex;

    /** The split function for the node. */
    DecisionFunction_Ptr m_splitter;

    //~~~~~~~~~~~~~~~~~~~~ CONSTRUCTORS ~~~~~~~~~~~~~~~~~~~~

    /**
     * \brief Constructs a node.
     *
     * \param maxReservoirSize      The maximum number of examples that can be stored in the node's reservoir.
     * \param randomNumberGenerator A random number generator.
     */
    Node(size_t maxReservoirSize, const tvgutil::RandomNumberGenerator_Ptr& randomNumberGenerator)
    : m_leftChildIndex(-1), m_reservoir(maxReservoirSize, randomNumberGenerator), m_rightChildIndex(-1)
    {}
  };

  //#################### PUBLIC TYPEDEFS ####################
public:
  typedef boost::shared_ptr<const DecisionFunctionGenerator<Label> > DecisionFunctionGenerator_CPtr;

  //#################### PRIVATE TYPEDEFS ####################
private:
  typedef boost::shared_ptr<const Example<Label> > Example_CPtr;
  typedef boost::shared_ptr<Node> Node_Ptr;
  typedef tvgutil::PriorityQueue<int,float,void*,std::greater<float> > SplittabilityQueue;

  //#################### PRIVATE VARIABLES ####################
private:
  /** A generator that can be used to pick appropriate decision functions for nodes. */
  DecisionFunctionGenerator_CPtr m_decisionFunctionGenerator;

  /** The indices of nodes to which examples have been added during the current call to add_examples() and whose splittability may need recalculating. */
  std::set<int> m_dirtyNodes;

  /** The maximum number of examples that can be stored in a node's reservoir. */
  size_t m_maxReservoirSize;

  /** The nodes in the tree. */
  std::vector<Node_Ptr> m_nodes;

  /** A random number generator. */
  tvgutil::RandomNumberGenerator_Ptr m_randomNumberGenerator;

  /** The root node's index in the node array. */
  int m_rootIndex;

  /** The minimum number of examples that must have been added to an example reservoir before its containing node can be split. */
  size_t m_seenExamplesThreshold;

  /** A priority queue of nodes that ranks them by how suitable they are for splitting. */
  SplittabilityQueue m_splittabilityQueue;

  //#################### CONSTRUCTORS ####################
public:
  /**
   * \brief Constructs an empty decision tree.
   *
   * \param maxReservoirSize          The maximum number of examples that can be stored in a node's reservoir.
   * \param seenExamplesThreshold     The minimum number of examples that must have been added to an example reservoir before its containing node can be split.
   * \param randomNumberGenerator     A random number generator.
   * \param decisionFunctionGenerator A generator that can be used to pick appropriate decision functions for nodes.
   */
  explicit DecisionTree(size_t maxReservoirSize, size_t seenExamplesThreshold, const tvgutil::RandomNumberGenerator_Ptr& randomNumberGenerator, const DecisionFunctionGenerator_CPtr& decisionFunctionGenerator)
  : m_decisionFunctionGenerator(decisionFunctionGenerator), m_maxReservoirSize(maxReservoirSize), m_randomNumberGenerator(randomNumberGenerator)
  {
    m_rootIndex = add_node();
  }

  //#################### PUBLIC MEMBER FUNCTIONS ####################
public:
  /**
   * \brief Adds new training examples to the decision tree.
   *
   * \param examples  The examples to be added.
   */
  void add_examples(const std::vector<Example_CPtr>& examples)
  {
    // Add each example to the tree.
    for(typename std::vector<Example_CPtr>::const_iterator it = examples.begin(), iend = examples.end(); it != iend; ++it)
    {
      add_example(*it);
    }

    // Update the splittability values for any nodes whose reservoirs were changed whilst adding examples.
    for(std::set<int>::const_iterator it = m_dirtyNodes.begin(), iend = m_dirtyNodes.end(); it != iend; ++it)
    {
      update_splittability(*it);
    }

    // Clear the list of dirty nodes once their splittability has been updated.
    m_dirtyNodes.clear();
  }

  /**
   * \brief Looks up the probability mass function for the leaf to which an example with the specified descriptor would be added.
   *
   * \param descriptor  The descriptor.
   * \return            The probability mass function for the leaf to which an example with that descriptor would be added.
   */
  ProbabilityMassFunction<Label> lookup_pmf(const Descriptor_CPtr& descriptor) const
  {
    int leafIndex = find_leaf(*descriptor);
    return make_pmf(leafIndex);
  }

  /**
   * \brief Outputs the decision tree to a stream.
   *
   * \param os  The stream to which to output the tree.
   */
  void output(std::ostream& os) const
  {
    output_subtree(os, m_rootIndex, "");
  }

  /**
   * \brief Trains the tree by splitting a number of suitable nodes (e.g. those that have a fairly full reservoir).
   *
   * The number of nodes that are split in each training step is limited to ensure that a step is not overly costly.
   *
   * \param splitBudget             The maximum number of nodes that may be split in this training step.
   * \param splittabilityThreshold  A threshold splittability below which nodes should not be split.
   */
  void train(size_t splitBudget, float splittabilityThreshold = 0.5f)
  {
    size_t nodesSplit = 0;

    // Keep splitting nodes until we either run out of nodes to split or exceed the split budget. In practice,
    // we will also stop splitting if the best node's splittability falls below a threshold. If the best node
    // cannot be split at present, we remove it from the queue to give the other nodes a chance and re-add it
    // at the end of the training step.
    std::vector<typename SplittabilityQueue::Element> elementsToReAdd;
    while(!m_splittabilityQueue.empty() && nodesSplit < splitBudget)
    {
      typename SplittabilityQueue::Element e = m_splittabilityQueue.top();
      if(e.key() >= splittabilityThreshold)
      {
        m_splittabilityQueue.pop();
        if(split_node(e.id())) ++nodesSplit;
        else elementsToReAdd.push_back(e);
      }
      else break;
    }

    // Re-add any elements corresponding to nodes that could not be successfully split in this training step.
    for(typename std::vector<typename SplittabilityQueue::Element>::iterator it = elementsToReAdd.begin(), iend = elementsToReAdd.end(); it != iend; ++it)
    {
      m_splittabilityQueue.insert(it->id(), it->key(), it->data());
    }
  }

  //#################### PRIVATE MEMBER FUNCTIONS ####################
private:
  /**
   * \brief Adds a new training example to the decision tree.
   *
   * \param example The example to be added.
   */
  void add_example(const Example_CPtr& example)
  {
    // Find the leaf to which to add the new example.
    int leafIndex = find_leaf(*example->get_descriptor());

    // Add the example to the leaf's reservoir.
    if(m_nodes[leafIndex]->m_reservoir.add_example(example))
    {
      // If the leaf's reservoir changed as a result of adding the example, record this fact to ensure that the leaf's splittability is properly recalculated.
      m_dirtyNodes.insert(leafIndex);
    }
  }

  /**
   * \brief Adds a node to the decision tree.
   *
   * \return The ID of the newly-added node.
   */
  int add_node()
  {
    m_nodes.push_back(Node_Ptr(new Node(m_maxReservoirSize, m_randomNumberGenerator)));
    int id = static_cast<int>(m_nodes.size()) - 1;
    m_splittabilityQueue.insert(id, 0.0f, NULL);
    return id;
  }

  /**
   * \brief Fills the specified reservoir with examples sampled from an input set of examples. 
   *
   * \param inputExamples The set of examples from which to sample.
   * \param multipliers   The per-class ratios between the total number of examples seen for a class and the number of examples currently in the source reservoir.
   * \param reservoir     The reservoir to fill.
   */
  void fill_reservoir(const std::vector<Example_CPtr>& inputExamples, const std::map<Label,float>& multipliers, ExampleReservoir<Label>& reservoir)
  {
    // Group the input examples by label.
    std::map<Label,std::vector<Example_CPtr> > inputExamplesByLabel;
    for(typename std::vector<Example_CPtr>::const_iterator it = inputExamples.begin(), iend = inputExamples.end(); it != iend; ++it)
    {
      inputExamplesByLabel[(*it)->get_label()].push_back(*it);
    }

    // For each group:
    for(typename std::map<Label,std::vector<Example_CPtr> >::const_iterator it = inputExamplesByLabel.begin(), iend = inputExamplesByLabel.end(); it != iend; ++it)
    {
#if 1
      // Sample the appropriate number of examples (based on the multiplier for that group) and add them to the target reservoir.
      float multiplier = multipliers.find(it->first)->second;
      size_t sampleCount = static_cast<size_t>(it->second.size() * multiplier + 0.5f);
      std::vector<Example_CPtr> sampledExamples = sample_examples(it->second, sampleCount);
      for(size_t j = 0; j < sampleCount; ++j)
      {
        reservoir.add_example(sampledExamples[j]);
      }
#else
      // Simply add all of the examples for the group to the target reservoir (useful for debugging purposes).
      for(size_t j = 0, size = it->second.size(); j < size; ++j)
      {
        reservoir.add_example(it->second[j]);
      }
#endif
    }
  }

  /**
   * \brief Finds the index of the leaf to which an example with the specified descriptor would currently be added.
   *
   * \param descriptor  The descriptor.
   * \return            The index of the leaf to which an example with the descriptor would currently be added.
   */
  int find_leaf(const Descriptor& descriptor) const
  {
    int curIndex = m_rootIndex;
    while(!is_leaf(curIndex))
    {
      curIndex = m_nodes[curIndex]->m_splitter->classify_descriptor(descriptor) == DecisionFunction::DC_LEFT ? m_nodes[curIndex]->m_leftChildIndex : m_nodes[curIndex]->m_rightChildIndex;
    }
    return curIndex;
  }

  /**
   * \brief Returns whether or not the specified node is a leaf.
   *
   * \param nodeIndex  The index of the node.
   * \return           true, if the specified node is a leaf, or false otherwise.
   */
  bool is_leaf(int nodeIndex) const
  {
    return m_nodes[nodeIndex]->m_leftChildIndex == -1;
  }

  /**
   * \brief Makes a probability mass function for the specified leaf.
   *
   * \param leafIndex The leaf for which to make the probability mass function.
   * \return          The probability mass function.
   */
  ProbabilityMassFunction<Label> make_pmf(int leafIndex) const
  {
    return ProbabilityMassFunction<Label>(*m_nodes[leafIndex]->m_reservoir.get_histogram());
  }

  /**
   * \brief Outputs a subtree of the decision tree to a stream.
   *
   * \param os                The stream to which to output the subtree.
   * \param subtreeRootIndex  The index of the node at the root of the subtree.
   * \param indent            An indentation string to use in order to offset the output of the subtree.
   */
  void output_subtree(std::ostream& os, int subtreeRootIndex, const std::string& indent) const
  {
    int leftChildIndex = m_nodes[subtreeRootIndex]->m_leftChildIndex;
    int rightChildIndex = m_nodes[subtreeRootIndex]->m_rightChildIndex;
    DecisionFunction_Ptr splitter = m_nodes[subtreeRootIndex]->m_splitter;

    // Output the current node.
    os << indent << subtreeRootIndex << ": ";
    if(splitter) os << *splitter;
    else os << m_nodes[subtreeRootIndex]->m_reservoir.seen_examples() << ' ' << make_pmf(subtreeRootIndex);
    os << '\n';

    // Recursively output any children of the current node.
    if(leftChildIndex != -1) output_subtree(os, leftChildIndex, indent + "  ");
    if(rightChildIndex != -1) output_subtree(os, rightChildIndex, indent + "  ");
  }

  /**
   * \brief Randomly samples sampleCount examples (with replacement) from the specified set of input examples.
   *
   * \param inputExamples The set of examples from which to sample.
   * \param sampleCount   The number of samples to choose.
   * \return              The chosen set of examples.
   */
  std::vector<Example_CPtr> sample_examples(const std::vector<Example_CPtr>& inputExamples, size_t sampleCount)
  {
    std::vector<Example_CPtr> outputExamples;
    for(size_t i = 0; i < sampleCount; ++i)
    {
      int exampleIndex = m_randomNumberGenerator->generate_int_in_range(0, static_cast<int>(inputExamples.size()) - 1);
      outputExamples.push_back(inputExamples[exampleIndex]);
    }
    return outputExamples;
  }

  /**
   * \brief Attempts to split the node with the specified index.
   *
   * \param nodeIndex The index of the node to try and split.
   * \return          true, if the node was successfully split, or false otherwise.
   */
  bool split_node(int nodeIndex)
  {
    const int CANDIDATE_COUNT = 5;
    const float GAIN_THRESHOLD = 0.0f;

    Node& n = *m_nodes[nodeIndex];
    typename DecisionFunctionGenerator<Label>::Split_CPtr split = m_decisionFunctionGenerator->split_examples(n.m_reservoir, CANDIDATE_COUNT, GAIN_THRESHOLD);
    if(!split) return false;

    // Set the decision function of the node to be split.
    n.m_splitter = split->m_decisionFunction;

    // Add left and right child nodes and populate their example reservoirs based on the chosen split.
    n.m_leftChildIndex = add_node();
    n.m_rightChildIndex = add_node();
    std::map<Label,float> multipliers = n.m_reservoir.get_class_multipliers();
    fill_reservoir(split->m_leftExamples, multipliers, m_nodes[n.m_leftChildIndex]->m_reservoir);
    fill_reservoir(split->m_rightExamples, multipliers, m_nodes[n.m_rightChildIndex]->m_reservoir);

    // Update the splittability for the child nodes.
    update_splittability(n.m_leftChildIndex);
    update_splittability(n.m_rightChildIndex);

    // Clear the example reservoir in the node that was split.
    n.m_reservoir.clear();

    return true;
  }

  /**
   * \brief Updates the splittability of the specified node.
   *
   * \param nodeIndex  The index of the node.
   */
  void update_splittability(int nodeIndex)
  {
    // Recalculate the node's splittability.
    const ExampleReservoir<Label>& reservoir = m_nodes[nodeIndex]->m_reservoir;
    float splittability = reservoir.seen_examples() >= m_seenExamplesThreshold ? ExampleUtil::calculate_entropy(*reservoir.get_histogram()) : 0.0f;

    // Update the splittability queue to reflect the node's new splittability.
    m_splittabilityQueue.update_key(nodeIndex, splittability);
  }
};

}

#endif
