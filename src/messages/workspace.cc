/* Copyright 2017-2018 ccls Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "clang_complete.hh"
#include "fuzzy_match.hh"
#include "log.hh"
#include "message_handler.hh"
#include "pipeline.hh"
#include "project.hh"
#include "query_utils.hh"

#include <llvm/ADT/STLExtras.h>

#include <algorithm>
#include <ctype.h>
#include <functional>
#include <limits.h>

namespace ccls {
MAKE_REFLECT_STRUCT(lsSymbolInformation, name, kind, location, containerName);

void MessageHandler::workspace_didChangeConfiguration(EmptyParam &) {
  for (const std::string &folder : g_config->workspaceFolders)
    project->Load(folder);
  project->Index(working_files, lsRequestId());

  clang_complete->FlushAllSessions();
};

void MessageHandler::workspace_didChangeWatchedFiles(
    DidChangeWatchedFilesParam &param) {
  for (auto &event : param.changes) {
    std::string path = event.uri.GetPath();
    IndexMode mode = working_files->GetFileByFilename(path)
                         ? IndexMode::Normal
                         : IndexMode::NonInteractive;
    switch (event.type) {
    case FileChangeType::Created:
    case FileChangeType::Changed: {
      pipeline::Index(path, {}, mode);
      if (mode == IndexMode::Normal)
        clang_complete->NotifySave(path);
      else
        clang_complete->OnClose(path);
      break;
    }
    case FileChangeType::Deleted:
      pipeline::Index(path, {}, mode);
      clang_complete->OnClose(path);
      break;
    }
  }
}

void MessageHandler::workspace_didChangeWorkspaceFolders(
    DidChangeWorkspaceFoldersParam &param) {
  for (const WorkspaceFolder &wf : param.event.removed) {
    std::string root = wf.uri.GetPath();
    EnsureEndsInSlash(root);
    LOG_S(INFO) << "delete workspace folder " << wf.name << ": " << root;
    auto it = llvm::find(g_config->workspaceFolders, root);
    if (it != g_config->workspaceFolders.end()) {
      g_config->workspaceFolders.erase(it);
      {
        // auto &folder = project->root2folder[path];
        // FIXME delete
      }
      project->root2folder.erase(root);
    }
  }
  for (const WorkspaceFolder &wf : param.event.added) {
    std::string root = wf.uri.GetPath();
    EnsureEndsInSlash(root);
    LOG_S(INFO) << "add workspace folder " << wf.name << ": " << root;
    g_config->workspaceFolders.push_back(root);
    project->Load(root);
  }

  project->Index(working_files, lsRequestId());

  clang_complete->FlushAllSessions();
}

namespace {
// Lookup |symbol| in |db| and insert the value into |result|.
bool AddSymbol(
    DB *db, WorkingFiles *working_files, SymbolIdx sym, bool use_detailed,
    std::vector<std::tuple<lsSymbolInformation, int, SymbolIdx>> *result) {
  std::optional<lsSymbolInformation> info = GetSymbolInfo(db, sym, true);
  if (!info)
    return false;

  Use loc;
  if (Maybe<DeclRef> dr = GetDefinitionSpell(db, sym))
    loc = *dr;
  else {
    auto decls = GetNonDefDeclarations(db, sym);
    if (decls.empty())
      return false;
    loc = decls[0];
  }

  std::optional<lsLocation> ls_location = GetLsLocation(db, working_files, loc);
  if (!ls_location)
    return false;
  info->location = *ls_location;
  result->emplace_back(*info, int(use_detailed), sym);
  return true;
}
} // namespace

void MessageHandler::workspace_symbol(WorkspaceSymbolParam &param,
                                      ReplyOnce &reply) {
  std::vector<lsSymbolInformation> result;
  std::string query = param.query;

  // {symbol info, matching detailed_name or short_name, index}
  std::vector<std::tuple<lsSymbolInformation, int, SymbolIdx>> cands;
  bool sensitive = g_config->workspaceSymbol.caseSensitivity;

  // Find subsequence matches.
  std::string query_without_space;
  query_without_space.reserve(query.size());
  for (char c : query)
    if (!isspace(c))
      query_without_space += c;

  auto Add = [&](SymbolIdx sym) {
    std::string_view detailed_name = db->GetSymbolName(sym, true);
    int pos = ReverseSubseqMatch(query_without_space, detailed_name, sensitive);
    return pos >= 0 &&
           AddSymbol(db, working_files, sym,
                     detailed_name.find(':', pos) != std::string::npos,
                     &cands) &&
           cands.size() >= g_config->workspaceSymbol.maxNum;
  };
  for (auto &func : db->funcs)
    if (Add({func.usr, SymbolKind::Func}))
      goto done_add;
  for (auto &type : db->types)
    if (Add({type.usr, SymbolKind::Type}))
      goto done_add;
  for (auto &var : db->vars)
    if (var.def.size() && !var.def[0].is_local() &&
        Add({var.usr, SymbolKind::Var}))
      goto done_add;
done_add:

  if (g_config->workspaceSymbol.sort && query.size() <= FuzzyMatcher::kMaxPat) {
    // Sort results with a fuzzy matching algorithm.
    int longest = 0;
    for (auto &cand : cands)
      longest = std::max(
          longest, int(db->GetSymbolName(std::get<2>(cand), true).size()));
    FuzzyMatcher fuzzy(query, g_config->workspaceSymbol.caseSensitivity);
    for (auto &cand : cands)
      std::get<1>(cand) =
          fuzzy.Match(db->GetSymbolName(std::get<2>(cand), std::get<1>(cand)));
    std::sort(cands.begin(), cands.end(), [](const auto &l, const auto &r) {
      return std::get<1>(l) > std::get<1>(r);
    });
    result.reserve(cands.size());
    for (auto &cand : cands) {
      // Discard awful candidates.
      if (std::get<1>(cand) <= FuzzyMatcher::kMinScore)
        break;
      result.push_back(std::get<0>(cand));
    }
  } else {
    result.reserve(cands.size());
    for (auto &cand : cands)
      result.push_back(std::get<0>(cand));
  }

  reply(result);
}
} // namespace ccls
