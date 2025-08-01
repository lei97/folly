/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

/**
 * Minimal addr2line using folly::symbolizer. Used for tests.
 *
 *    folly-addr2line -e objectfile --demangle <addr1> [<addr2> ...]
 */

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

#include <folly/Range.h>
#include <folly/debugging/symbolizer/Dwarf.h>
#include <folly/debugging/symbolizer/Elf.h>
#include <folly/debugging/symbolizer/SymbolizedFrame.h>
#include <folly/debugging/symbolizer/Symbolizer.h>
#include <folly/init/Init.h>
#include <folly/portability/GFlags.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

DEFINE_string(e, "", "Path to ELF object file (.so, binary)");
DEFINE_bool(demangle, false, "Degmangle symbols");
DEFINE_bool(C, false, "Degmangle symbols");
DEFINE_bool(a, false, "Show addresses");
DEFINE_bool(i, false, "Unwind inlined functions");
DEFINE_bool(f, false, "Show function names");

using namespace folly::symbolizer;

bool shouldDemangle() {
  return FLAGS_demangle || FLAGS_C;
}

void addr2line(std::shared_ptr<ElfFile> elfFile, uintptr_t address) {
  if (elfFile->getSectionContainingAddress(address) == nullptr) {
    if (FLAGS_a) {
      std::cout << "0x" << std::hex << address << std::dec << "\n";
    }
    if (FLAGS_f) {
      std::cout << "??\n";
    }
    std::cout << "??:0\n";
    return;
  }

  std::array<SymbolizedFrame, 100> frames;
  frames[0].found = true;
  frames[0].addr = address;
  frames[0].file = elfFile;
  frames[0].name =
      elfFile->getSymbolName(elfFile->getDefinitionByAddress(address));

  ElfCache cache;
  Dwarf(&cache, elfFile.get())
      .findAddress(
          address,
          FLAGS_i ? LocationInfoMode::FULL_WITH_INLINE : LocationInfoMode::FULL,
          frames[0],
          /* extraInlineFrames */ folly::range(frames).subpiece(1));

  size_t n = 0;
  while (frames[n].found) {
    n++;
  }
  if (!FLAGS_i) {
    CHECK_LE(n, 1);
  }

  // Inlined frames [1,n) are filled in deepest call first order,
  // but frames[0] is used for the non-inlined caller.
  // Move frames[0] to the end so the entire array is ordered.
  std::rotate(&frames[0], &frames[1], &frames[0] + n);

  CHECK_NE(n, 0);
  if (FLAGS_a) {
    std::cout << "0x" << std::hex << address << std::dec << '\n';
  }
  for (size_t i = 0; i < n; i++) {
    const auto& f = frames[i];
    if (FLAGS_f) {
      std::cout
          << (f.name ? (shouldDemangle() ? folly::demangle(f.name) : f.name)
                     : "??")
          << '\n';
    }
    auto path = f.location.file.toString();
    path = path.empty() ? "??" : path;
    std::cout << path << ":" << f.location.line << '\n';
  }
}

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv);
  auto elfFile = std::make_shared<ElfFile>(FLAGS_e.c_str());

  if (argc == 1) {
    for (std::string line; std::getline(std::cin, line);) {
      addr2line(elfFile, strtoll(line.c_str(), nullptr, 0));
    }
  } else {
    for (int i = 1; i < argc; i++) {
      addr2line(elfFile, strtoll(argv[i], nullptr, 0));
    }
  }

  return 0;
}

#else // FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

int main(int, char*[]) {
  return 1;
}

#endif // FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF
