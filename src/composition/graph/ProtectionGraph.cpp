#include <composition/graph/ProtectionGraph.hpp>
#include <composition/graph/algorithm/all_cycles.hpp>
#include <composition/graph/constraint/dependency.hpp>
#include <composition/graph/constraint/present.hpp>
#include <composition/graph/constraint/preserved.hpp>
#include <lemon/connectivity.h>

namespace composition::graph {
using composition::graph::algorithm::AllCycles;
using composition::graph::constraint::Dependency;
using composition::graph::constraint::Present;
using composition::graph::constraint::PresentConstraint;
using composition::graph::constraint::Preserved;
using composition::graph::constraint::PreservedConstraint;
using llvm::dbgs;
using llvm::dyn_cast;

ProtectionGraph::ProtectionGraph() {
  vertices = std::make_unique<lemon::ListDigraph::NodeMap<vertex_t>>(LG);
  edges = std::make_unique<lemon::ListDigraph::ArcMap<edge_t>>(LG);
}

void ProtectionGraph::addManifests(std::set<Manifest*> manifests) {
  size_t total = manifests.size();
  dbgs() << "Adding " << std::to_string(total) << " manifests to protection graph\n";

  size_t i = 0;
  for (auto& m : manifests) {
    dbgs() << "#" << std::to_string(i++) << "/" << std::to_string(total) << "\r";
    addManifest(m);
  }

  dbgs() << "#" << std::to_string(i) << "/" << std::to_string(total) << "\n";
}

void ProtectionGraph::addManifest(Manifest* m) {
  m->Clean();
  MANIFESTS.insert({m->index, m});
  for (auto& c : m->constraints) {
    addConstraint(m->index, c);
  }
}

vertex_idx_t ProtectionGraph::add_vertex(llvm::Value* value, bool shadow) {
  if (auto vFound = vertexCache[shadow]->find(value);
      vFound != vertexCache[shadow]->end() && VERTICES_DESCRIPTORS.find(vFound->second) != VERTICES_DESCRIPTORS.end()) {
    return vFound->second;
  }
  vertex_idx_t idx = VertexIdx++;

  auto vd = LG.addNode();
  (*vertices)[vd] = vertex_t(idx, value, llvmToVertexName(value), llvmToVertexType(value));
  if (shadow) {
    (*vertices)[vd].name += "_shadow";
  }
  VERTICES_DESCRIPTORS.insert({idx, vd});
  vertexCache[shadow]->insert({value, idx});
  return idx;
}

vertex_idx_t ProtectionGraph::add_vertex(
    llvm::Value* value,
    const std::unordered_map<constraint::constraint_idx_t, std::shared_ptr<constraint::Constraint>>& constraints) {
  vertex_idx_t idx = add_vertex(value, false);
  auto& v = (*vertices)[VERTICES_DESCRIPTORS.at(idx)];

  for (auto& [cIdx, c] : constraints) {
    v.constraints.insert({cIdx, c});
    CONSTRAINTS_VERTICES.insert({cIdx, idx});
  }
  return idx;
}

edge_idx_t ProtectionGraph::add_edge(vertex_idx_t sIdx, vertex_idx_t dIdx) {
  assert(sIdx != dIdx);
  auto source = VERTICES_DESCRIPTORS.at(sIdx);
  auto destination = VERTICES_DESCRIPTORS.at(dIdx);
  assert(source != destination);

  if (auto ed = lemon::findArc(LG, source, destination); ed != lemon::INVALID) {
    return (*edges)[ed].index;
  }

  auto ed = LG.addArc(source, destination);
  edge_idx_t idx = EdgeIdx++;
  (*edges)[ed] = edge_t{idx};
  EDGES_DESCRIPTORS.insert({idx, ed});
  return idx;
}

edge_idx_t ProtectionGraph::add_edge(
    vertex_idx_t sIdx, vertex_idx_t dIdx,
    const std::unordered_map<constraint::constraint_idx_t, std::shared_ptr<constraint::Constraint>>& constraints) {
  edge_idx_t idx = add_edge(sIdx, dIdx);
  auto& e = (*edges)[EDGES_DESCRIPTORS.at(idx)];

  for (auto& [cIdx, c] : constraints) {
    e.constraints.insert({cIdx, c});
    CONSTRAINTS_EDGES.insert({ConstraintIdx, EdgeIdx});
  }
  return idx;
}

size_t ProtectionGraph::countVertices() { return lemon::countNodes(LG); }
size_t ProtectionGraph::countEdges() { return lemon::countArcs(LG); }

constraint_idx_t ProtectionGraph::addConstraint(manifest_idx_t idx, std::shared_ptr<Constraint> c) {
  MANIFESTS_CONSTRAINTS.insert({idx, ConstraintIdx});
  if (auto d = dyn_cast<Dependency>(c.get())) {
    auto dstNode = add_vertex(d->getFrom(), true);
    auto srcNode = add_vertex(d->getTo(), false);
    add_edge(srcNode, dstNode, {{ConstraintIdx, c}});
  } else if (auto present = dyn_cast<Present>(c.get())) {
    add_vertex(present->getTarget(), {{ConstraintIdx, c}});
  } else if (auto preserved = dyn_cast<Preserved>(c.get())) {
    add_vertex(preserved->getTarget(), {{ConstraintIdx, c}});
  } else if (auto tr = dyn_cast<True>(c.get())) {
    add_vertex(tr->getTarget(), {{ConstraintIdx, c}});
  } else {
    llvm_unreachable("Constraint unknown!");
  }
  return ConstraintIdx++;
}

void addToConstraintsMaps(const vertex_t& v, PresentConstraint& present, PreservedConstraint& preserved,
                          std::set<constraint_idx_t>& presentConstraints,
                          std::set<constraint_idx_t>& preservedConstraints) {
  if (v.constraints.empty()) {
    return;
  }

  for (auto& [cIdx, c] : v.constraints) {
    if (auto* p1 = llvm::dyn_cast<Present>(c.get())) {
      presentConstraints.insert(cIdx);
      present = p1->isInverse() ? present | PresentConstraint::NOT_PRESENT : present | PresentConstraint::PRESENT;
    } else if (auto* p2 = llvm::dyn_cast<Preserved>(c.get())) {
      preservedConstraints.insert(cIdx);
      preserved =
          p2->isInverse() ? preserved | PreservedConstraint::NOT_PRESERVED : preserved | PreservedConstraint::PRESERVED;
    }
  }
}

std::set<std::pair<manifest_idx_t, manifest_idx_t>> ProtectionGraph::vertexConflicts() {
  std::set<std::pair<manifest_idx_t, manifest_idx_t>> conflicts{};

  for (lemon::ListDigraph::NodeIt n(LG); n != lemon::INVALID; ++n) {
    PresentConstraint present = PresentConstraint::NONE;
    PreservedConstraint preserved = PreservedConstraint::NONE;

    std::set<constraint_idx_t> presentConstraints{};
    std::set<constraint_idx_t> preservedConstraints{};
    vertex_t& v = (*vertices)[n];
    addToConstraintsMaps(v, present, preserved, presentConstraints, preservedConstraints);

    auto addParentBB = [&, this](llvm::BasicBlock* BB) {
      if (BB->getParent() != nullptr) {
        llvm::Function* F = BB->getParent();
        if (auto vFound = vertexCache[false]->find(F); vFound != vertexCache[false]->end()) {
          vertex_t& v = (*vertices)[VERTICES_DESCRIPTORS.at(vFound->second)];
          addToConstraintsMaps(v, present, preserved, presentConstraints, preservedConstraints);
        }
      }
    };

    auto addParentI = [&, this](llvm::Instruction* I) {
      if (I->getParent() != nullptr) {
        llvm::BasicBlock* BB = I->getParent();
        if (auto vFound = vertexCache[false]->find(BB); vFound != vertexCache[false]->end()) {
          vertex_t& v = (*vertices)[VERTICES_DESCRIPTORS.at(vFound->second)];
          addToConstraintsMaps(v, present, preserved, presentConstraints, preservedConstraints);
          addParentBB(BB);
        }
      }
    };

    if (auto I = llvm::dyn_cast<llvm::Instruction>(v.value)) {
      addParentI(I);
    } else if (auto BB = llvm::dyn_cast<llvm::BasicBlock>(v.value)) {
      addParentBB(BB);
    }

    if (present == PresentConstraint::CONFLICT) {
      for (auto i = presentConstraints.begin(), i_end = presentConstraints.end(); i != i_end; ++i) {
        for (auto j = std::next(i, 1), j_end = presentConstraints.end(); j != j_end; ++j) {
          manifest_idx_t m1 = MANIFESTS_CONSTRAINTS.right.at(*i);
          manifest_idx_t m2 = MANIFESTS_CONSTRAINTS.right.at(*j);

          conflicts.insert({m1, m2});
        }
      }
    }

    if (preserved == PreservedConstraint::CONFLICT) {
      for (auto i = preservedConstraints.begin(), i_end = preservedConstraints.end(); i != i_end; ++i) {
        for (auto j = std::next(i, 1), j_end = preservedConstraints.end(); j != j_end; ++j) {
          manifest_idx_t m1 = MANIFESTS_CONSTRAINTS.right.at(*i);
          manifest_idx_t m2 = MANIFESTS_CONSTRAINTS.right.at(*j);

          conflicts.insert({m1, m2});
        }
      }
    }
  }

  return conflicts;
}

std::set<std::pair<manifest_idx_t, manifest_idx_t>> ProtectionGraph::computeDependencies() {
  std::set<std::pair<manifest_idx_t, manifest_idx_t>> dependencies{};

  for (auto& [mIdx, m] : MANIFESTS) {
    // Manifest dependencies
    for (auto [it, it_end] = DependencyUndo.right.equal_range(mIdx); it != it_end; ++it) {
      dependencies.insert({it->second, mIdx});
    }
  }

  return dependencies;
}

std::set<std::set<manifest_idx_t>> ProtectionGraph::computeCycles() {
  Profiler detectingProfiler{};

  if (lemon::dag(LG)) {
    return {};
  }

  std::set<std::set<manifest_idx_t>> cycles{};

  /*llvm::dbgs() << "Cycles...\n";
  AllCycles a{};
  llvm::dbgs() << "Nodes: " << lemon::countNodes(LG) << " Edges: " << lemon::countArcs(LG) << "\n";
  std::set<std::set<lemon::ListDigraph::Node>> all = a.simpleCycles(LG);
  llvm::dbgs() << "End...\n";*/

  lemon::ListDigraph::NodeMap<int> components{LG};
  const int numComponents = lemon::stronglyConnectedComponents(LG, components);

  std::map<int, std::set<lemon::ListDigraph::Node>> sccs{};
  for (lemon::ListDigraph::NodeIt scNode(LG); scNode != lemon::INVALID; ++scNode) {
    sccs[components[scNode]].insert(scNode);
  }

  for (auto& [sccId, scc] : sccs) {
    if (scc.size() == 1) {
      continue;
    } else {
      std::set<manifest_idx_t> cycle{};

      for (auto& s : scc) {
        for (lemon::ListDigraph::OutArcIt e(LG, s); e != lemon::INVALID; ++e) {
          assert(LG.target(e) != s);
          if (scc.find(LG.target(e)) == scc.end()) {
            continue;
          }
          const edge_t& ed = (*edges)[e];
          for (auto& [cIdx, c] : ed.constraints) {
            if (auto mFound = MANIFESTS_CONSTRAINTS.right.find(cIdx); mFound != MANIFESTS_CONSTRAINTS.right.end()) {
              cycle.insert(mFound->second);
            }
          }
        }
        for (lemon::ListDigraph::InArcIt e(LG, s); e != lemon::INVALID; ++e) {
          assert(LG.source(e) != s);
          if (scc.find(LG.source(e)) == scc.end()) {
            continue;
          }
          const edge_t& ed = (*edges)[e];
          for (auto& [cIdx, c] : ed.constraints) {
            if (auto mFound = MANIFESTS_CONSTRAINTS.right.find(cIdx); mFound != MANIFESTS_CONSTRAINTS.right.end()) {
              cycle.insert(mFound->second);
            }
          }
        }
      }

      if (cycle.size() > 1) {
        cycles.insert(cycle);
      }
    }
  }
  cStats.timeConflictDetection += detectingProfiler.stop();

  return cycles;
}

std::set<Manifest*> ProtectionGraph::conflictHandling(llvm::Module& M) {
  Profiler detectingProfiler{};
  auto conflicts = vertexConflicts();
  auto dependencies = computeDependencies();
  cStats.timeConflictDetection += detectingProfiler.stop();
  auto cycles = computeCycles();

  Profiler resolvingProfiler{};
  bool hasCycles = false;
  do {
    boost::bimaps::bimap<int, manifest_idx_t> colsToM{};

    std::vector<int> rows{};
    std::vector<int> cols{};
    std::vector<double> coeffs{};

    // cost function
    auto cost = [](Manifest* m) -> double { return 1; };
    auto conflict = [&](glp_prob* lp, std::pair<manifest_idx_t, manifest_idx_t> pair) {
      // m1 and m2 conflict; m1 + m2 <= 1
      auto row = glp_add_rows(lp, 1);
      glp_set_row_bnds(lp, row, GLP_UP, 0.0, 1.0);
      std::ostringstream os;
      os << "conflict_" << pair.first << "_" << pair.second;
      glp_set_row_name(lp, row, os.str().c_str());

      rows.push_back(row);
      cols.push_back(colsToM.right.at(pair.first));
      coeffs.push_back(1.0);

      rows.push_back(row);
      cols.push_back(colsToM.right.at(pair.second));
      coeffs.push_back(1.0);
    };
    auto dependency = [&](glp_prob* lp, std::pair<manifest_idx_t, manifest_idx_t> pair) {
      // m1 depends on m2; m1 <= m2; m1 - m2 <= 0
      auto row = glp_add_rows(lp, 1);
      glp_set_row_bnds(lp, row, GLP_UP, 0.0, 0.0);
      std::ostringstream os;
      os << "dependency_" << pair.first << "_" << pair.second;
      glp_set_row_name(lp, row, os.str().c_str());

      rows.push_back(row);
      cols.push_back(colsToM.right.at(pair.first));
      coeffs.push_back(1.0);

      rows.push_back(row);
      cols.push_back(colsToM.right.at(pair.second));
      coeffs.push_back(-1.0);
    };

    int cycleCount = 0;
    auto cycle = [&](glp_prob* lp, std::set<manifest_idx_t> ms) {
      // m1..mN form a cycle; m1+m2+..+mN <= N-1
      auto row = glp_add_rows(lp, 1);
      glp_set_row_bnds(lp, row, GLP_UP, 0.0, ms.size() - 1);
      std::ostringstream os;
      os << "cycle_" << cycleCount++;
      glp_set_row_name(lp, row, os.str().c_str());

      for (auto& idx : ms) {
        rows.push_back(row);
        cols.push_back(colsToM.right.at(idx));
        coeffs.push_back(1.0);
      }
    };

    auto lp = glp_create_prob();     // creates a problem object
    glp_set_prob_name(lp, "sample"); // assigns a symbolic name to the problem object
    glp_set_obj_dir(lp, GLP_MIN);

    // ROWS
    glp_add_rows(lp, 3); // adds three rows to the problem object
    // row 1
    glp_set_row_name(lp, 1, "explicit");          // assigns name p to first row
    glp_set_row_bnds(lp, 1, GLP_LO, 6000.0, 0.0); // 0 < explicit <= inf
    // row 2
    glp_set_row_name(lp, 2, "implicit");       // assigns name q to second row
    glp_set_row_bnds(lp, 2, GLP_LO, 0.0, 0.0); // 0 < implicit <= inf
    // row 3
    glp_set_row_name(lp, 3, "unique");         // assigns name q to second row
    glp_set_row_bnds(lp, 3, GLP_LO, 0.0, 0.0); // 0 < unique <= inf

    // COLUMNS
    for (auto& [mIdx, m] : MANIFESTS) {
      // column N
      std::ostringstream os;
      os << "m" << m->index;
      auto col = glp_add_cols(lp, 1);
      glp_set_col_name(lp, col, os.str().c_str()); // assigns name m_n to nth column
      glp_set_col_kind(lp, col, GLP_BV);           // values are binary
      glp_set_col_bnds(lp, col, GLP_DB, 0.0, 1.0); // values are binary
      glp_set_obj_coef(lp, col, cost(m));          // costs

      colsToM.insert({col, m->index});

      // explicit
      rows.push_back(1);
      cols.push_back(col);
      coeffs.push_back(m->Coverage().size());

      // implicit
      rows.push_back(2);
      cols.push_back(col);
      coeffs.push_back(0);

      // unique
      rows.push_back(3);
      cols.push_back(col);
      coeffs.push_back(0);
    }

    // Add dependencies
    for (auto&& pair : dependencies) {
      dependency(lp, pair);
    }

    // Add conflicts
    for (auto&& pair : conflicts) {
      conflict(lp, pair);
    }

    // Add cycles
    for (auto&& c : cycles) {
      cycle(lp, c);
    }

    size_t dataSize = rows.size();
    // now prepend the required position zero placeholder (any value will do but zero is safe)
    // first create length one vectors using default member construction
    std::vector<int> iav(1, 0);
    std::vector<int> jav(1, 0);
    std::vector<double> arv(1, 0);

    // then concatenate these with the original data vectors
    iav.insert(iav.end(), rows.begin(), rows.end());
    jav.insert(jav.end(), cols.begin(), cols.end());
    arv.insert(arv.end(), coeffs.begin(), coeffs.end());

    glp_load_matrix(lp, dataSize, &iav[0], &jav[0], &arv[0]); // calls the routine glp_load_matrix
    glp_write_lp(lp, NULL, "prob.glp");

    glp_simplex(lp, NULL); // calls the routine glp_simplex to solve LP problem
    glp_intopt(lp, NULL);
    auto total_cost = glp_mip_obj_val(lp);
    glp_print_mip(lp, "sol.glp");

    std::set<Manifest*> accepted{};
    for (auto& [col, mIdx] : colsToM) {
      if (glp_mip_col_val(lp, col) == 1) {
        accepted.insert(MANIFESTS.at(mIdx));
      }
    }
    glp_delete_prob(lp);

    ProtectionGraph pg{};
    pg.addManifests(accepted);
    pg.addHierarchy(M);
    pg.connectShadowNodes();
    auto newCycles = pg.computeCycles();
    if (!newCycles.empty()) {
      hasCycles = true;
      for (auto& c : newCycles) {
        cycles.insert(c);
      }
    } else {
      cStats.cycles = cycles.size();
      cStats.conflicts = conflicts.size();
      cStats.timeConflictResolving += resolvingProfiler.stop();
      return accepted;
    }
  } while (hasCycles);

  llvm_unreachable("Reached end of conflict handling");
}

std::vector<Manifest*> ProtectionGraph::topologicalSortManifests(std::set<Manifest*> manifests) {
  std::set<Manifest*> all{manifests.begin(), manifests.end()};
  std::set<Manifest*> seen{};

  for (lemon::ListDigraph::NodeIt n(LG); n != lemon::INVALID; ++n) {
    for (lemon::ListDigraph::InArcIt ei(LG, n); ei != lemon::INVALID; ++ei) {
      const edge_t& e = (*edges)[ei];
      for (auto& [cIdx, c] : e.constraints) {
        auto it = MANIFESTS_CONSTRAINTS.right.find(cIdx);
        if (it == MANIFESTS_CONSTRAINTS.right.end()) {
          continue;
        }
        manifest_idx_t idx = it->second;
        seen.insert(MANIFESTS.find(idx)->second);
      }
    }
  }

  std::vector<Manifest*> result{};
  std::set_difference(all.begin(), all.end(), seen.begin(), seen.end(), std::back_inserter(result));

  seen.clear();
  lemon::ListDigraph::NodeMap<int> order{LG};
  assert(lemon::checkedTopologicalSort(LG, order) == true);

  const auto nodes = static_cast<unsigned long>(lemon::countNodes(LG));
  std::vector<lemon::ListDigraph::Node> sorted(nodes);

  for (lemon::ListDigraph::NodeIt n(LG); n != lemon::INVALID; ++n) {
    sorted[order[n]] = static_cast<lemon::ListDigraph::Node>(n);
  }

  for (lemon::ListDigraph::Node& n : sorted) {
    for (lemon::ListDigraph::InArcIt ei(LG, n); ei != lemon::INVALID; ++ei) {
      const edge_t& e = (*edges)[ei];
      edge_idx_t idx = e.index;
      for (auto& [cIdx, c] : e.constraints) {
        auto mFound = MANIFESTS_CONSTRAINTS.right.find(cIdx);
        if (mFound == MANIFESTS_CONSTRAINTS.right.end()) {
          continue;
        }
        Manifest* manifest = MANIFESTS.at(mFound->second);

        if (seen.find(manifest) != seen.end()) {
          continue;
        }
        seen.insert(manifest);
        result.push_back(manifest);
      }
    }
  }

  return result;
}

void ProtectionGraph::addHierarchy(llvm::Module& M) {
  for (auto&& F : M) {
    auto fNode = add_vertex(&F, false);
    for (auto&& BB : F) {
      auto bbNode = add_vertex(&BB, false);
      add_edge(bbNode, fNode, {{ConstraintIdx++, std::make_shared<Dependency>("hierarchy", &BB, &F)}});
      for (auto&& I : BB) {
        auto iNode = add_vertex(&I, false);
        add_edge(iNode, bbNode, {{ConstraintIdx++, std::make_shared<Dependency>("hierarchy", &I, &BB)}});
      }
    }
  }
}

void ProtectionGraph::connectShadowNodes() {
  for (const auto& [value, sIdx] : vertexShadowCache) {
    add_edge(sIdx, add_vertex(value, false));
    if (auto* F = llvm::dyn_cast<llvm::Function>(value)) {
      for (auto&& BB : *F) {
        add_edge(sIdx, add_vertex(&BB, false));
        for (auto&& I : BB) {
          add_edge(sIdx, add_vertex(&I, false));
        }
      }
    } else if (auto* BB = llvm::dyn_cast<llvm::BasicBlock>(value)) {
      for (auto&& I : *BB) {
        add_edge(sIdx, add_vertex(&I, false));
      }
    }
  }
}

void ProtectionGraph::destroy() {
  vertexRealCache.clear();
  vertexShadowCache.clear();
  DependencyUndo.clear();
}

void ProtectionGraph::computeManifestDependencies() {
  ManifestUndoMap undo{};
  for (auto& [idx, m] : MANIFESTS) {
    for (auto it : m->UndoValues()) {
      auto worked = undo.insert({idx, it});
      assert(worked.second && "undo");
    }
  }
  std::unordered_map<manifest_idx_t, std::unordered_set<llvm::Value*>> manifestUsers{};

  for (auto& [idx, u] : undo.left) {
    for (auto it = u->user_begin(), it_end = u->user_end(); it != it_end; ++it) {
      manifestUsers[idx].insert(*it);
    }
  }

  for (auto& [idx, u] : undo.left) {
    std::unordered_set<manifest_idx_t> manifests{};
    Manifest* m = MANIFESTS.find(idx)->second;

    for (auto I : m->Coverage()) {
      for (auto [it, it_end] = undo.right.equal_range(I); it != it_end; ++it) {
        manifests.insert(it->second);
      }
    }
    for (auto m2 : manifests) {
      ManifestProtection.insert({m2, idx});
    }
  }

  for (auto& [m, users] : manifestUsers) {
    for (auto u : users) {
      for (auto [it, it_end] = undo.right.equal_range(u); it != it_end; ++it) {
        if (m != it->second) {
          DependencyUndo.insert({it->second, m});
        }
      }
    }
  }
}

} // namespace composition::graph