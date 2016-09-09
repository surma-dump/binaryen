/*
 * Copyright 2015 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Removes branches for which we go to where they go anyhow
//

#include <wasm.h>
#include <pass.h>
#include <ast_utils.h>
#include <wasm-builder.h>

namespace wasm {

struct RemoveUnusedBrs : public WalkerPass<PostWalker<RemoveUnusedBrs, Visitor<RemoveUnusedBrs>>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new RemoveUnusedBrs; }

  bool anotherCycle;

  // Whether a value can flow in the current path. If so, then a br with value
  // can be turned into a value, which will flow through blocks/ifs to the right place
  bool valueCanFlow;

  typedef std::vector<Expression**> Flows;

  // list of breaks that are currently flowing. if they reach their target without
  // interference, they can be removed (or their value forwarded TODO)
  Flows flows;

  // a stack for if-else contents, we merge their outputs
  std::vector<Flows> ifStack;

  // list of all loops, so we can optimize them
  std::vector<Loop*> loops;

  static void visitAny(RemoveUnusedBrs* self, Expression** currp) {
    auto* curr = *currp;
    auto& flows = self->flows;

    if (curr->is<Break>()) {
      flows.clear();
      auto* br = curr->cast<Break>();
      if (!br->condition) { // TODO: optimize?
        // a break, let's see where it flows to
        flows.push_back(currp);
        self->valueCanFlow = true; // start optimistic
      } else {
        self->valueCanFlow = false;
      }
    } else if (curr->is<Return>()) {
      flows.clear();
      flows.push_back(currp);
      self->valueCanFlow = true; // start optimistic
    } else if (curr->is<If>()) {
      auto* iff = curr->cast<If>();
      if (iff->ifFalse) {
        assert(self->ifStack.size() > 0);
        for (auto* flow : self->ifStack.back()) {
          flows.push_back(flow);
        }
        self->ifStack.pop_back();
      }
    } else if (curr->is<Block>()) {
      // any breaks flowing to here are unnecessary, as we get here anyhow
      auto* block = curr->cast<Block>();
      auto name = block->name;
      if (name.is()) {
        size_t size = flows.size();
        size_t skip = 0;
        for (size_t i = 0; i < size; i++) {
          auto* flow = (*flows[i])->dynCast<Break>();
          if (flow && flow->name == name) {
            if (!flow->value || self->valueCanFlow) {
              if (!flow->value) {
                // br => nop
                ExpressionManipulator::nop<Break>(flow);
              } else {
                // br with value => value
                *flows[i] = flow->value;
              }
              skip++;
              self->anotherCycle = true;
            }
          } else if (skip > 0) {
            flows[i - skip] = flows[i];
          }
        }
        if (skip > 0) {
          flows.resize(size - skip);
        }
        // drop a nop at the end of a block, which prevents a value flowing
        while (block->list.size() > 0 && block->list.back()->is<Nop>()) {
          block->list.resize(block->list.size() - 1);
          self->anotherCycle = true;
        }
      }
    } else if (curr->is<Nop>()) {
      // ignore (could be result of a previous cycle)
      self->valueCanFlow = false;
    } else {
      // anything else stops the flow
      flows.clear();
      self->valueCanFlow = false;
    }
  }

  static void clear(RemoveUnusedBrs* self, Expression** currp) {
    self->flows.clear();
  }

  static void saveIfTrue(RemoveUnusedBrs* self, Expression** currp) {
    self->ifStack.push_back(std::move(self->flows));
  }

  void visitLoop(Loop* curr) {
    loops.push_back(curr);
  }

  void visitIf(If* curr) {
    if (!curr->ifFalse) {
      // if without an else. try to reduce   if (condition) br  =>  br_if (condition)
      Break* br = curr->ifTrue->dynCast<Break>();
      if (br && !br->condition) { // TODO: if there is a condition, join them
        // if the br has a value, then if => br_if means we always execute the value, and also the order is value,condition vs condition,value
        if (br->value) {
          EffectAnalyzer value(br->value);
          if (value.hasSideEffects()) return;
          EffectAnalyzer condition(curr->condition);
          if (condition.invalidates(value)) return;
        }
        br->condition = curr->condition;
        replaceCurrent(br);
        anotherCycle = true;
      }
    }
    // TODO: if-else can be turned into a br_if as well, if one of the sides is a dead end
  }

  // override scan to add a pre and a post check task to all nodes
  static void scan(RemoveUnusedBrs* self, Expression** currp) {
    self->pushTask(visitAny, currp);

    auto* iff = (*currp)->dynCast<If>();

    if (iff) {
      self->pushTask(doVisitIf, currp);
      if (iff->ifFalse) {
      // we need to join up if-else control flow, and clear after the condition
        self->pushTask(scan, &iff->ifFalse);
        self->pushTask(saveIfTrue, currp); // safe the ifTrue flow, we'll join it later
      }
      self->pushTask(scan, &iff->ifTrue);
      self->pushTask(clear, currp); // clear all flow after the condition
      self->pushTask(scan, &iff->condition);
    } else {
      WalkerPass<PostWalker<RemoveUnusedBrs, Visitor<RemoveUnusedBrs>>>::scan(self, currp);
    }
  }

  // optimizes a loop. returns true if we made changes
  bool optimizeLoop(Loop* loop) {
    // if a loop ends in
    // (loop $in
    //   (block $out
    //     if (..) br $in; else br $out;
    //   )
    // )
    // then our normal opts can remove the break out since it flows directly out
    // (and later passes make the if one-armed). however, the simple analysis
    // fails on patterns like
    //     if (..) br $out;
    //     br $in;
    // which is a common way to do a while (1) loop (end it with a jump to the
    // top), so we handle that here. Specifically we want to conditionalize
    // breaks to the loop top, i.e., put them behind a condition, so that other
    // code can flow directly out and thus brs out can be removed. (even if
    // the change is to let a break somewhere else flow out, that can still be
    // helpful, as it shortens the logical loop. it is also good to generate
    // an if-else instead of an if, as it might allow an eqz to be removed
    // by flipping arms)
    if (!loop->name.is()) return false;
    auto* block = loop->body->dynCast<Block>();
    if (!block) return false;
    // does the last element break to the top of the loop?
    auto& list = block->list;
    if (list.size() <= 1) return false;
    auto* last = list.back()->dynCast<Break>();
    if (!last || !ExpressionAnalyzer::isSimple(last) || last->name != loop->name) return false;
    // last is a simple break to the top of the loop. if we can conditionalize it,
    // it won't block things from flowing out and not needing breaks to do so.
    Index i = list.size() - 2;
    Builder builder(*getModule());
    while (1) {
      auto* curr = list[i];
      if (auto* iff = curr->dynCast<If>()) {
        // let's try to move the code going to the top of the loop into the if-else
        if (!iff->ifFalse) {
          // we need the ifTrue to break, so it cannot reach the code we want to move
          if (ExpressionAnalyzer::getEndingSimpleBreak(iff->ifTrue)) {
            iff->ifFalse = builder.stealSlice(block, i + 1, list.size());
            return true;
          }
        } else {
          // this is already an if-else. if one side is a dead end, we can append to the other, if
          // there is no returned value to concern us
          assert(!isConcreteWasmType(iff->type)); // can't be, since in the middle of a block
          if (ExpressionAnalyzer::getEndingSimpleBreak(iff->ifTrue)) {
            iff->ifFalse = builder.blockifyMerge(iff->ifFalse, builder.stealSlice(block, i + 1, list.size()));
            return true;
          } else if (ExpressionAnalyzer::getEndingSimpleBreak(iff->ifFalse)) {
            iff->ifTrue = builder.blockifyMerge(iff->ifTrue, builder.stealSlice(block, i + 1, list.size()));
            return true;
          }
        }
        return false;
      } else if (auto* brIf = curr->dynCast<Break>()) {
        // br_if is similar to if.
        if (brIf->condition && !brIf->value && brIf->name != loop->name) {
          if (i == list.size() - 2) {
            // there is the br_if, and then the br to the top, so just flip them and the condition
            brIf->condition = builder.makeUnary(EqZInt32, brIf->condition);
            last->name = brIf->name;
            brIf->name = loop->name;
            return true;
          } else {
            // there are elements in the middle,
            //   br_if $somewhere (condition)
            //   (..more..)
            //   br $in
            // we can convert the br_if to an if. this has a cost, though,
            // so only do it if it looks useful, which it definitely is if
            //  (a) $somewhere is straight out (so the br out vanishes), and
            //  (b) this br_if is the only branch to that block (so the block will vanish)
            if (brIf->name == block->name && BreakSeeker::count(block, block->name) == 1) {
              // note that we could drop the last element here, it is a br we know for sure is removable,
              // but telling stealSlice to steal all to the end is more efficient, it can just truncate.
              list[i] = builder.makeIf(brIf->condition, builder.makeBreak(brIf->name), builder.stealSlice(block, i + 1, list.size()));
              return true;
            }
          }
        }
        return false;
      }
      // if there is control flow, we must stop looking
      if (EffectAnalyzer(curr).branches) {
        return false;
      }
      if (i == 0) return false;
      i--;
    }
  }

  void doWalkFunction(Function* func) {
    // multiple cycles may be needed
    bool worked = false;
    do {
      anotherCycle = false;
      WalkerPass<PostWalker<RemoveUnusedBrs, Visitor<RemoveUnusedBrs>>>::doWalkFunction(func);
      assert(ifStack.empty());
      // flows may contain returns, which are flowing out and so can be optimized
      for (size_t i = 0; i < flows.size(); i++) {
        auto* flow = (*flows[i])->cast<Return>(); // cannot be a break
        if (!flow->value) {
          // return => nop
          ExpressionManipulator::nop(flow);
          anotherCycle = true;
        } else if (valueCanFlow) {
          // return with value => value
          *flows[i] = flow->value;
          anotherCycle = true;
        }
      }
      flows.clear();
      // optimize loops (we don't do it while tracking flows, as they can interfere)
      for (auto* loop : loops) {
        anotherCycle |= optimizeLoop(loop);
      }
      loops.clear();
      if (anotherCycle) worked = true;
    } while (anotherCycle);
    // finally, we may have simplified ifs enough to turn them into selects
    struct Selectifier : public WalkerPass<PostWalker<Selectifier, Visitor<Selectifier>>> {
      void visitIf(If* curr) {
        if (curr->ifFalse && isConcreteWasmType(curr->ifTrue->type) && isConcreteWasmType(curr->ifFalse->type)) {
          // if with else, consider turning it into a select if there is no control flow
          // TODO: estimate cost
          EffectAnalyzer condition(curr->condition);
          if (!condition.hasSideEffects()) {
            EffectAnalyzer ifTrue(curr->ifTrue);
            if (!ifTrue.hasSideEffects()) {
              EffectAnalyzer ifFalse(curr->ifFalse);
              if (!ifFalse.hasSideEffects()) {
                auto* select = getModule()->allocator.alloc<Select>();
                select->condition = curr->condition;
                select->ifTrue = curr->ifTrue;
                select->ifFalse = curr->ifFalse;
                select->finalize();
                replaceCurrent(select);
              }
            }
          }
        }
      }
    };
    Selectifier selectifier;
    selectifier.setModule(getModule());
    selectifier.walkFunction(func);
    if (worked) {
      // Our work may alter block and if types, they may now return
      struct TypeUpdater : public WalkerPass<PostWalker<TypeUpdater, Visitor<TypeUpdater>>> {
        void visitBlock(Block* curr) {
          curr->finalize();
        }
        void visitLoop(Loop* curr) {
          curr->finalize();
        }
        void visitIf(If* curr) {
          curr->finalize();
        }
      };
      TypeUpdater typeUpdater;
      typeUpdater.walkFunction(func);
    }
  }
};

Pass *createRemoveUnusedBrsPass() {
  return new RemoveUnusedBrs();
}

} // namespace wasm
