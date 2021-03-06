/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2012 Marco Costalba, Joona Kiiski, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <iostream>

#include "movegen.h"
#include "search.h"
#include "thread.h"
#include "ucioption.h"

using namespace Search;

ThreadPool Threads; // Global object

namespace { extern "C" {

 // start_routine() is the C function which is called when a new thread
 // is launched. It is a wrapper to the virtual function idle_loop().

 long start_routine(Thread* th) { th->idle_loop(); return 0; }

} }


// Thread c'tor starts a newly-created thread of execution that will call
// the the virtual function idle_loop(), going immediately to sleep.

Thread::Thread() : splitPoints() {

  searching = exit = false;
  maxPly = splitPointsSize = 0;
  activeSplitPoint = NULL;
  idx = Threads.size();

  if (!thread_create(handle, start_routine, this))
  {
      std::cerr << "Failed to create thread number " << idx << std::endl;
      ::exit(EXIT_FAILURE);
  }
}


// Thread d'tor waits for thread termination before to return

Thread::~Thread() {

  exit = true; // Search must be already finished
  notify_one();
  thread_join(handle); // Wait for thread termination
}


// TimerThread::idle_loop() is where the timer thread waits msec milliseconds
// and then calls check_time(). If msec is 0 thread sleeps until is woken up.
extern void check_time();

void TimerThread::idle_loop() {

  while (!exit)
  {
      mutex.lock();

      if (!exit)
          sleepCondition.wait_for(mutex, msec ? msec : INT_MAX);

      mutex.unlock();

      if (msec)
          check_time();
  }
}


// MainThread::idle_loop() is where the main thread is parked waiting to be started
// when there is a new search. Main thread will launch all the slave threads.

void MainThread::idle_loop() {

  while (true)
  {
      mutex.lock();

      thinking = false;

      while (!thinking && !exit)
      {
          Threads.sleepCondition.notify_one(); // Wake up UI thread if needed
          sleepCondition.wait(mutex);
      }

      mutex.unlock();

      if (exit)
          return;

      searching = true;

      Search::think();

      assert(searching);

      searching = false;
  }
}


// Thread::notify_one() wakes up the thread when there is some search to do

void Thread::notify_one() {

  mutex.lock();
  sleepCondition.notify_one();
  mutex.unlock();
}


// Thread::wait_for() set the thread to sleep until condition 'b' turns true

void Thread::wait_for(volatile const bool& b) {

  mutex.lock();
  while (!b) sleepCondition.wait(mutex);
  mutex.unlock();
}


// Thread::cutoff_occurred() checks whether a beta cutoff has occurred in the
// current active split point, or in some ancestor of the split point.

bool Thread::cutoff_occurred() const {

  for (SplitPoint* sp = activeSplitPoint; sp; sp = sp->parent)
      if (sp->cutoff)
          return true;

  return false;
}


// Thread::is_available_to() checks whether the thread is available to help the
// thread 'master' at a split point. An obvious requirement is that thread must
// be idle. With more than two threads, this is not sufficient: If the thread is
// the master of some split point, it is only available as a slave to the slaves
// which are busy searching the split point at the top of slaves split point
// stack (the "helpful master concept" in YBWC terminology).

bool Thread::is_available_to(Thread* master) const {

  if (searching)
      return false;

  // Make a local copy to be sure doesn't become zero under our feet while
  // testing next condition and so leading to an out of bound access.
  int size = splitPointsSize;

  // No split points means that the thread is available as a slave for any
  // other thread otherwise apply the "helpful master" concept if possible.
  return !size || (splitPoints[size - 1].slavesMask & (1ULL << master->idx));
}


// init() is called at startup. Initializes lock and condition variable and
// launches requested threads sending them immediately to sleep. We cannot use
// a c'tor becuase Threads is a static object and we need a fully initialized
// engine at this point due to allocation of endgames in Thread c'tor.

void ThreadPool::init() {

  sleepWhileIdle = true;
  timer = new TimerThread();
  threads.push_back(new MainThread());
  read_uci_options();
}


// exit() cleanly terminates the threads before the program exits.

void ThreadPool::exit() {

  delete timer; // As first becuase check_time() accesses threads data

  for (size_t i = 0; i < threads.size(); i++)
      delete threads[i];
}


// read_uci_options() updates internal threads parameters from the corresponding
// UCI options and creates/destroys threads to match the requested number. Thread
// objects are dynamically allocated to avoid creating in advance all possible
// threads, with included pawns and material tables, if only few are used.

void ThreadPool::read_uci_options() {

  maxThreadsPerSplitPoint = Options["Max Threads per Split Point"];
  minimumSplitDepth       = Options["Min Split Depth"] * ONE_PLY;
  size_t requested        = Options["Threads"];

  assert(requested > 0);

  while (threads.size() < requested)
      threads.push_back(new Thread());

  while (threads.size() > requested)
  {
      delete threads.back();
      threads.pop_back();
  }
}


// slave_available() tries to find an idle thread which is available as a slave
// for the thread 'master'.

bool ThreadPool::slave_available(Thread* master) const {

  for (size_t i = 0; i < threads.size(); i++)
      if (threads[i]->is_available_to(master))
          return true;

  return false;
}


// split() does the actual work of distributing the work at a node between
// several available threads. If it does not succeed in splitting the node
// (because no idle threads are available, or because we have no unused split
// point objects), the function immediately returns. If splitting is possible, a
// SplitPoint object is initialized with all the data that must be copied to the
// helper threads and then helper threads are told that they have been assigned
// work. This will cause them to instantly leave their idle loops and call
// search(). When all threads have returned from search() then split() returns.

template <bool Fake>
Value ThreadPool::split(Position& pos, Stack* ss, Value alpha, Value beta,
                        Value bestValue, Move* bestMove, Depth depth, Move threatMove,
                        int moveCount, MovePicker& mp, int nodeType) {

  assert(pos.pos_is_ok());
  assert(bestValue > -VALUE_INFINITE);
  assert(bestValue <= alpha);
  assert(alpha < beta);
  assert(beta <= VALUE_INFINITE);
  assert(depth > DEPTH_ZERO);

  Thread* master = pos.this_thread();

  if (master->splitPointsSize >= MAX_SPLITPOINTS_PER_THREAD)
      return bestValue;

  // Pick the next available split point from the split point stack
  SplitPoint& sp = master->splitPoints[master->splitPointsSize];

  sp.master = master;
  sp.parent = master->activeSplitPoint;
  sp.slavesMask = 1ULL << master->idx;
  sp.depth = depth;
  sp.bestMove = *bestMove;
  sp.threatMove = threatMove;
  sp.alpha = alpha;
  sp.beta = beta;
  sp.nodeType = nodeType;
  sp.bestValue = bestValue;
  sp.mp = &mp;
  sp.moveCount = moveCount;
  sp.pos = &pos;
  sp.nodes = 0;
  sp.cutoff = false;
  sp.ss = ss;

  master->activeSplitPoint = &sp;
  int slavesCnt = 0;

  assert(master->searching);

  // Try to allocate available threads and ask them to start searching setting
  // 'searching' flag. This must be done under lock protection to avoid concurrent
  // allocation of the same slave by another master.
  mutex.lock();
  sp.mutex.lock();

  for (size_t i = 0; i < threads.size() && !Fake; ++i)
      if (threads[i]->is_available_to(master))
      {
          sp.slavesMask |= 1ULL << i;
          threads[i]->activeSplitPoint = &sp;
          threads[i]->searching = true; // Slave leaves idle_loop()
          threads[i]->notify_one(); // Could be sleeping

          if (++slavesCnt + 1 >= maxThreadsPerSplitPoint) // Include master
              break;
      }

  master->splitPointsSize++;

  sp.mutex.unlock();
  mutex.unlock();

  // Everything is set up. The master thread enters the idle loop, from which
  // it will instantly launch a search, because its 'searching' flag is set.
  // The thread will return from the idle loop when all slaves have finished
  // their work at this split point.
  if (slavesCnt || Fake)
  {
      master->Thread::idle_loop(); // Force a call to base class idle_loop()

      // In helpful master concept a master can help only a sub-tree of its split
      // point, and because here is all finished is not possible master is booked.
      assert(!master->searching);
  }

  // We have returned from the idle loop, which means that all threads are
  // finished. Note that setting 'searching' and decreasing splitPointsSize is
  // done under lock protection to avoid a race with Thread::is_available_to().
  mutex.lock();
  sp.mutex.lock();

  master->searching = true;
  master->splitPointsSize--;
  master->activeSplitPoint = sp.parent;
  pos.set_nodes_searched(pos.nodes_searched() + sp.nodes);
  *bestMove = sp.bestMove;

  sp.mutex.unlock();
  mutex.unlock();

  return sp.bestValue;
}

// Explicit template instantiations
template Value ThreadPool::split<false>(Position&, Stack*, Value, Value, Value, Move*, Depth, Move, int, MovePicker&, int);
template Value ThreadPool::split<true>(Position&, Stack*, Value, Value, Value, Move*, Depth, Move, int, MovePicker&, int);


// wait_for_think_finished() waits for main thread to go to sleep then returns

void ThreadPool::wait_for_think_finished() {

  MainThread* t = main_thread();
  t->mutex.lock();
  while (t->thinking) sleepCondition.wait(t->mutex);
  t->mutex.unlock();
}


// start_thinking() wakes up the main thread sleeping in  main_loop() so to start
// a new search, then returns immediately.

void ThreadPool::start_thinking(const Position& pos, const LimitsType& limits,
                                const std::vector<Move>& searchMoves, StateStackPtr& states) {
  wait_for_think_finished();

  SearchTime = Time::now(); // As early as possible

  Signals.stopOnPonderhit = Signals.firstRootMove = false;
  Signals.stop = Signals.failedLowAtRoot = false;

  RootPos = pos;
  Limits = limits;
  SetupStates = states; // Ownership transfer here
  RootMoves.clear();

  for (MoveList<LEGAL> ml(pos); !ml.end(); ++ml)
      if (searchMoves.empty() || count(searchMoves.begin(), searchMoves.end(), ml.move()))
          RootMoves.push_back(RootMove(ml.move()));

  main_thread()->thinking = true;
  main_thread()->notify_one(); // Starts main thread
}
