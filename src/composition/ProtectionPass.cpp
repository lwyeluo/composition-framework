#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <composition/GraphPass.hpp>
#include <composition/ProtectionPass.hpp>
#include <composition/trace/PreservedValueRegistry.hpp>
#include <composition/metric/Stats.hpp>

using namespace llvm;
namespace composition {

void ProtectionPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<GraphPass>();
}

bool ProtectionPass::runOnModule(llvm::Module &M) {
  dbgs() << "ProtectionPass running\n";

  auto &pass = getAnalysis<GraphPass>();
  auto manifests = pass.GetManifestsInOrder();

  dbgs() << "Got " << manifests.size() << " manifests\n";

  for (auto &m : manifests) {
    m->Redo();
  }

  Stats s{};
  s.collect(&M, manifests);
  s.dump(dbgs());

  PreservedValueRegistry::Clear();
  return !manifests.empty();
}

char ProtectionPass::ID = 0;
static llvm::RegisterPass<ProtectionPass> X("constraint-protection", "Constraint Protection Pass", true, false);
}