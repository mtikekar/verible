// Copyright 2017-2020 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <functional>
#include <iostream>
#include <string>

#include "absl/flags/usage.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "common/util/file_util.h"
#include "common/util/init_command_line.h"
#include "common/util/subcommand.h"
#include "verilog/transform/strip_comments.h"

using verible::SubcommandArgsRange;
using verible::SubcommandEntry;

static absl::Status StripComments(const SubcommandArgsRange& args,
                                  std::istream&, std::ostream& outs,
                                  std::ostream&) {
  if (args.empty()) {
    return absl::InvalidArgumentError(
        "Missing file argument.  Use '-' for stdin.");
  }
  const char* source_file = args[0];
  std::string source_contents;
  if (auto status = verible::file::GetContents(source_file, &source_contents);
      !status.ok()) {
    return status;
  }

  char replace_char;
  if (args.size() == 1) {
    replace_char = ' ';
  } else if (args.size() == 2) {
    absl::string_view replace_str(args[1]);
    if (replace_str.empty())
      replace_char = '\0';
    else if (replace_str.length() == 1)
      replace_char = replace_str[0];
    else
      return absl::InvalidArgumentError(
          "Replacement must be a single character.");
  } else {
    return absl::InvalidArgumentError("Too many arguments.");
  }

  verilog::StripVerilogComments(source_contents, &outs, replace_char);

  return absl::OkStatus();
}

static const std::pair<absl::string_view, SubcommandEntry> kCommands[] = {
    {"strip-comments",  //
     {&StripComments,   //
      R"(strip-comments file [replacement-char]

Inputs:
  'file' is a Verilog or SystemVerilog source file.
  Use '-' to read from stdin.

  'replacement-char' is a character to replace comments with.
  If not given, or given as a single space character, the comment contents and
  delimiters are replaced with spaces.
  If an empty string, the comment contents and delimiters are deleted. Newlines
  are not deleted.
  If a single character, the comment contents are replaced with the character.

Output: (stdout)
  Contents of original file with // and /**/ comments removed.
)"}},
};

int main(int argc, char* argv[]) {
  // Create a registry of subcommands (locally, rather than as a static global).
  verible::SubcommandRegistry commands;
  for (const auto& entry : kCommands) {
    const auto status = commands.RegisterCommand(entry.first, entry.second);
    if (!status.ok()) {
      std::cerr << status.message() << std::endl;
      return 2;  // fatal error
    }
  }

  const std::string usage = absl::StrCat("usage: ", argv[0],
                                         " command args...\n"
                                         "available commands:\n",
                                         commands.ListCommands());

  // Process invocation args.
  const auto args = verible::InitCommandLine(usage, &argc, &argv);
  if (args.size() == 1) {
    std::cerr << absl::ProgramUsageMessage() << std::endl;
    return 1;
  }
  // args[0] is the program name
  // args[1] is the subcommand
  // subcommand args start at [2]
  const SubcommandArgsRange command_args(args.cbegin() + 2, args.cend());

  const auto& sub = commands.GetSubcommandEntry(args[1]);
  // Run the subcommand.
  const auto status = sub.main(command_args, std::cin, std::cout, std::cerr);
  if (!status.ok()) {
    std::cerr << status.message() << std::endl;
    return 1;
  }
  return 0;
}
