// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/game/error_display.h"
#include <cassert>
#include <thread>

int main() {
  Jadyx::ErrorDisplay display;
  int completed = 0;
  const auto owner = std::this_thread::get_id();
  auto callback = [&] {
    assert(std::this_thread::get_id() == owner);
    ++completed;
    // Callbacks must be outside the queue lock.
    (void)display.Current();
  };
  std::thread producer{
      [&] { display.Show("2001-0007", "main\n\ndetail", callback); }};
  producer.join();
  auto first = display.Current();
  assert(first && first->code == "2001-0007" &&
         first->message == "main\n\ndetail");
  display.Show("2002-0008", "", callback);
  display.Dismiss(first->id + 100);
  display.CompleteDismissed();
  assert(completed == 0);
  display.Dismiss(first->id);
  display.Dismiss(first->id);
  assert(!display.Current());
  display.CompleteDismissed();
  display.CompleteDismissed();
  assert(completed == 1);
  const auto second = display.Current();
  assert(second && second->id != first->id);
  display.Dismiss(first->id); // Stale UI cannot dismiss the next request.
  assert(display.Current());
  display.Close();
  display.CompleteDismissed();
  assert(completed == 2 && !display.Current());
  display.Show("2000-0000", "shutdown", callback);
  display.Stop();
  display.CompleteDismissed();
  display.Show("2000-0000", "late producer", callback);
  assert(completed == 2 && !display.Current());
}
