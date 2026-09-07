#include "test_utils.h"

#include <string>

#include <misuc/engine/core/UndoStack.h>
#include <misuc/engine/project/Session.h>

using misuc::core::UndoStack;
using misuc::project::Session;

TEST_CASE(undo_basic_roundtrip) {
  UndoStack<Session> u;
  Session a;
  a.setName("A");
  a.setTempo(100.0);
  Session b = a;
  b.setName("B");
  b.setTempo(120.0);
  Session c = b;
  c.setName("C");

  u.push(a);
  u.push(b);
  u.push(c);
  TEST_ASSERT(u.state().name() == "C");

  TEST_ASSERT(u.canUndo());
  TEST_ASSERT(u.undo());
  TEST_ASSERT(u.state().name() == "B");
  TEST_ASSERT(u.state().tempo() == 120.0);
  TEST_ASSERT(u.undo());
  TEST_ASSERT(u.state().name() == "A");
  TEST_ASSERT(!u.canUndo());
  TEST_ASSERT(!u.undo());

  TEST_ASSERT(u.canRedo());
  TEST_ASSERT(u.redo());
  TEST_ASSERT(u.state().name() == "B");
  TEST_ASSERT(u.redo());
  TEST_ASSERT(u.state().name() == "C");
  TEST_ASSERT(!u.canRedo());
  TEST_ASSERT(!u.redo());
}

TEST_CASE(undo_push_truncates_redo) {
  UndoStack<Session> u;
  Session a;
  a.setName("A");
  Session b = a;
  b.setName("B");
  Session c = b;
  c.setName("C");
  Session d = c;
  d.setName("D");

  u.push(a);
  u.push(b);
  u.push(c);
  u.undo(); // now at B, redo tail is C
  TEST_ASSERT(u.canRedo());

  u.push(d); // must discard C; D becomes the head
  TEST_ASSERT(u.state().name() == "D");
  TEST_ASSERT(!u.canRedo());
  TEST_ASSERT(u.undo());
  TEST_ASSERT(u.state().name() == "B");
  TEST_ASSERT(u.canRedo()); // redo leads back to D
  TEST_ASSERT(u.redo());
  TEST_ASSERT(u.state().name() == "D");
}

TEST_CASE(undo_capacity_bounds_history) {
  UndoStack<int> u(4);
  for (int i = 0; i < 10; ++i) {
    u.push(i);
  }
  TEST_ASSERT(u.depth() == 4);
  TEST_ASSERT(u.state() == 9);
  TEST_ASSERT(u.undo());
  TEST_ASSERT(u.state() == 8);
  TEST_ASSERT(u.undo());
  TEST_ASSERT(u.state() == 7);
  TEST_ASSERT(u.undo());
  TEST_ASSERT(u.state() == 6);
  TEST_ASSERT(!u.canUndo());
}

TEST_CASE(undo_empty_stack) {
  UndoStack<Session> u;
  TEST_ASSERT(!u.undo());
  TEST_ASSERT(!u.redo());
  TEST_ASSERT(!u.canUndo());
  TEST_ASSERT(!u.canRedo());
  u.clear();
  TEST_ASSERT(!u.canUndo());
}

TEST_CASE(undo_clear_resets) {
  UndoStack<Session> u;
  Session s;
  s.setName("x");
  u.push(s);
  TEST_ASSERT(!u.canUndo()); // one snapshot: nothing before it yet
  Session t = s;
  t.setName("y");
  u.push(t);
  TEST_ASSERT(u.canUndo()); // two snapshots: can step back to x
  u.clear();
  TEST_ASSERT(!u.canUndo());
  TEST_ASSERT(!u.canRedo());
  TEST_ASSERT(u.depth() == 0);
}