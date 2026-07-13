#include "cli/args.hpp"

#include <cctype>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "util/errors.hpp"
#include "util/pathutil.hpp"

namespace cidx {
namespace cli {
namespace {

// ---------------------------------------------------------------------------
// Usage / help text — transcribed VERBATIM from the Python tool
// (python3 -m indexer, Python 3.14 argparse, COLUMNS=80). Do not re-wrap.
// ---------------------------------------------------------------------------

const char kTopUsage[] =
    "usage: cidx [-h] [--version]\n"
    "            {init,import,index,resolve,search,analyze,db,component,repo,dir,file,symbol,graph,ast,cache}\n"
    "            ...\n";

const char kTopHelp[] =
    "usage: cidx [-h] [--version]\n"
    "            {init,import,index,resolve,search,analyze,db,component,repo,dir,file,symbol,graph,ast,cache}\n"
    "            ...\n"
    "\n"
    "cidx command-line skeleton\n"
    "\n"
    "positional arguments:\n"
    "  {init,import,index,resolve,search,analyze,db,component,repo,dir,file,symbol,graph,ast,cache}\n"
    "    init                create a blank index database\n"
    "    import              import a compile_commands.json\n"
    "    index               index imported C/C++ files\n"
    "    resolve             finalize cross-repo edges and roll up edge counts\n"
    "    search              fuzzy-search symbols by qualified name\n"
    "    analyze             run Souffle Datalog analyses over the index\n"
    "    db                  database maintenance (migrate, verify)\n"
    "    component           manage components (add, list, show, set-version,\n"
    "                        compile-commands, rm)\n"
    "    repo                group components into repositories; switch clones\n"
    "    dir                 browse or delete indexed directories\n"
    "    file                manage indexed files (list, show, flags, set, rm)\n"
    "    symbol              inspect indexed symbols (list, show, rm)\n"
    "    graph               query the relationship graph (callers, callees, refs,\n"
    "                        neighbors, walk, path, hierarchy, dispatch)\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --version             show program's version number and exit\n";

const char kInitUsage[] =
    "usage: cidx init [-h] [--force]\n";

const char kInitHelp[] =
    "usage: cidx init [-h] [--force]\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --force     overwrite an existing index database\n";

const char kImportUsage[] =
    "usage: cidx import [-h] --db DB [--name NAME] [--repo REPO] [--force]\n"
    "                   [--no-alias]\n";

const char kImportHelp[] =
    "usage: cidx import [-h] --db DB [--name NAME] [--repo REPO] [--force]\n"
    "                   [--no-alias]\n"
    "\n"
    "options:\n"
    "  -h, --help   show this help message and exit\n"
    "  --db DB      compile_commands.json (or the directory holding it)\n"
    "  --name NAME  component name override\n"
    "  --repo REPO  repository name to group the imported components under\n"
    "               (default: the git/dir-derived name)\n"
    "  --force      reimport: delete the existing component (its files and indexed\n"
    "               symbols) before importing\n"
    "  --no-alias   do not rewrite include paths to <label> tokens via the registry\n";

const char kIndexUsage[] =
    "usage: cidx index [-h] [--source COMPONENT] [--no-graph]\n"
    "                  [--no-autoderive-labels]\n"
    "                  [files ...]\n";

const char kIndexHelp[] =
    "usage: cidx index [-h] [--source COMPONENT] [--no-graph]\n"
    "                  [--no-autoderive-labels]\n"
    "                  [files ...]\n"
    "\n"
    "positional arguments:\n"
    "  files                 restrict to these files (default: all pending)\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --source COMPONENT    resolve relative FILE paths against this component's\n"
    "                        root\n"
    "  --no-graph            skip relationship-graph extraction (calls, inherits,\n"
    "                        …)\n"
    "  --no-autoderive-labels\n"
    "                        disable label autoderive fallback at parse time\n";

const char kResolveUsage[] =
    "usage: cidx resolve [-h]\n";

const char kResolveHelp[] =
    "usage: cidx resolve [-h]\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n";

const char kSearchUsage[] =
    "usage: cidx search [-h]\n"
    "                   [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                   [--limit N]\n"
    "                   pattern\n";

const char kSearchHelp[] =
    "usage: cidx search [-h]\n"
    "                   [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                   [--limit N]\n"
    "                   pattern\n"
    "\n"
    "positional arguments:\n"
    "  pattern               '::'-separated substrings matched in order, e.g.\n"
    "                        'conf::set' hits RdKafka::Conf::set\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict to one symbol kind\n"
    "  --limit N             show at most N matches (0 = all; default 25)\n";

const char kAnalyzeUsage[] =
    "usage: cidx analyze [-h] [--rule NAME] [--rules-file FILE] [--list]\n"
    "                    [--export-facts DIR] [--jobs N] [--db PATH]\n";

const char kAnalyzeHelp[] =
    "usage: cidx analyze [-h] [--rule NAME] [--rules-file FILE] [--list]\n"
    "                    [--export-facts DIR] [--jobs N] [--db PATH]\n"
    "\n"
    "options:\n"
    "  -h, --help          show this help message and exit\n"
    "  --rule NAME         built-in rule to run (see --list)\n"
    "  --rules-file FILE   user Souffle .dl program; the fact declarations are\n"
    "                      prepended automatically\n"
    "  --list              list the built-in rules as JSON\n"
    "  --export-facts DIR  write TSV fact files and the cidx_facts.dl prelude to\n"
    "                      DIR\n"
    "  --jobs N            Souffle worker count (default 1)\n"
    "  --db PATH           index database (default: the standard cache index)\n";

const char kDbUsage[] =
    "usage: cidx db [-h] {migrate,verify} ...\n";

const char kDbHelp[] =
    "usage: cidx db [-h] {migrate,verify} ...\n"
    "\n"
    "positional arguments:\n"
    "  {migrate,verify}\n"
    "    migrate         upgrade an existing index to the current schema (in place,\n"
    "                    no re-index)\n"
    "    verify          check that component roots and files exist on disk\n"
    "\n"
    "options:\n"
    "  -h, --help        show this help message and exit\n";

const char kDbMigrateUsage[] =
    "usage: cidx db migrate [-h] [--db PATH]\n";

const char kDbMigrateHelp[] =
    "usage: cidx db migrate [-h] [--db PATH]\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --db PATH   index database (default: the standard cache index)\n";

const char kDbVerifyUsage[] =
    "usage: cidx db verify [-h] [--component NAME] [--all] [--db PATH]\n";

const char kDbVerifyHelp[] =
    "usage: cidx db verify [-h] [--component NAME] [--all] [--db PATH]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --component NAME, -c NAME\n"
    "                        restrict to one component (default: all)\n"
    "  --all                 also list files that exist (default: only failures)\n"
    "  --db PATH             index database (default: the standard cache index)\n";

const char kComponentUsage[] =
    "usage: cidx component [-h]\n"
    "                      {add,list,ls,show,set-version,compile-commands,rm} ...\n";

const char kComponentHelp[] =
    "usage: cidx component [-h]\n"
    "                      {add,list,ls,show,set-version,compile-commands,rm} ...\n"
    "\n"
    "positional arguments:\n"
    "  {add,list,ls,show,set-version,compile-commands,rm}\n"
    "    add                 register a component\n"
    "    list (ls)           list registered components\n"
    "    show                show details for a component\n"
    "    set-version         set or clear a component's version\n"
    "    compile-commands    emit a compile_commands.json for the component\n"
    "    rm                  delete a component and everything indexed from it\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n";

const char kComponentAddUsage[] =
    "usage: cidx component add [-h] --path PATH [--name NAME] [--repo REPO]\n"
    "                          [--kind {repo,external}] [--no-git] [--version V]\n"
    "                          [--no-detect-version]\n";

const char kComponentAddHelp[] =
    "usage: cidx component add [-h] --path PATH [--name NAME] [--repo REPO]\n"
    "                          [--kind {repo,external}] [--no-git] [--version V]\n"
    "                          [--no-detect-version]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --path PATH           repo root or library header dir\n"
    "  --name NAME           component name (default: from .git/config)\n"
    "  --repo REPO           repository name to group under (default: component\n"
    "                        name)\n"
    "  --kind {repo,external}\n"
    "  --no-git              use --path as-is; do not promote to the enclosing git\n"
    "                        root\n"
    "  --version V           set component version to V (overrides auto-detection;\n"
    "                        '' clears)\n"
    "  --no-detect-version   disable trailing-segment version detection\n";

const char kComponentListUsage[] =
    "usage: cidx component list [-h] [--kind {repo,external}] [pattern]\n";

const char kComponentListHelp[] =
    "usage: cidx component list [-h] [--kind {repo,external}] [pattern]\n"
    "\n"
    "positional arguments:\n"
    "  pattern               optional free-text fuzzy filter: characters must\n"
    "                        appear in order, e.g. 'shp' matches shapes.c\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --kind {repo,external}\n"
    "                        restrict to one component kind\n";

const char kComponentShowUsage[] =
    "usage: cidx component show [-h] [--db PATH] NAME\n";

const char kComponentShowHelp[] =
    "usage: cidx component show [-h] [--db PATH] NAME\n"
    "\n"
    "positional arguments:\n"
    "  NAME        component name\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --db PATH   index database (default: the standard cache index)\n";

const char kComponentSetVersionUsage[] =
    "usage: cidx component set-version [-h] [--db PATH] NAME [V]\n";

const char kComponentSetVersionHelp[] =
    "usage: cidx component set-version [-h] [--db PATH] NAME [V]\n"
    "\n"
    "positional arguments:\n"
    "  NAME        component name\n"
    "  V           version string; omit or pass '' to clear\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --db PATH   index database (default: the standard cache index)\n";

const char kComponentCcUsage[] =
    "usage: cidx component compile-commands [-h] [--db PATH] COMPONENT\n";

const char kComponentCcHelp[] =
    "usage: cidx component compile-commands [-h] [--db PATH] COMPONENT\n"
    "\n"
    "positional arguments:\n"
    "  COMPONENT   component whose files to emit\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --db PATH   operate on this index DB (default: the standard index)\n";

const char kComponentRmUsage[] =
    "usage: cidx component rm [-h] (--id ID | --name NAME | --path PATH)\n"
    "                         [--dry-run]\n";

const char kComponentRmHelp[] =
    "usage: cidx component rm [-h] (--id ID | --name NAME | --path PATH)\n"
    "                         [--dry-run]\n"
    "\n"
    "options:\n"
    "  -h, --help   show this help message and exit\n"
    "  --id ID      component id\n"
    "  --name NAME  component name\n"
    "  --path PATH  component root path\n"
    "  --dry-run    preview the matches without deleting anything\n";

const char kRepoUsage[] =
    "usage: cidx repo [-h] {list,ls,show,add-clone,switch,realias,rm} ...\n";

const char kRepoHelp[] =
    "usage: cidx repo [-h] {list,ls,show,add-clone,switch,realias,rm} ...\n"
    "\n"
    "positional arguments:\n"
    "  {list,ls,show,add-clone,switch,realias,rm}\n"
    "    list (ls)           list repositories\n"
    "    show                show a repository's clones and components\n"
    "    add-clone           register another checkout directory\n"
    "    switch              rebase the repository onto another clone (by\n"
    "                        path/label)\n"
    "    realias             rewrite stored include paths to <label> tokens via the\n"
    "                        registry\n"
    "    rm                  remove a repository\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n";

const char kRepoListUsage[] =
    "usage: cidx repo list [-h] [--kind {repo,external}] [--db PATH] [pattern]\n";

const char kRepoListHelp[] =
    "usage: cidx repo list [-h] [--kind {repo,external}] [--db PATH] [pattern]\n"
    "\n"
    "positional arguments:\n"
    "  pattern               fuzzy-filter by repository name\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --kind {repo,external}\n"
    "                        filter by repository kind\n"
    "  --db PATH             index database (default: the standard cache index)\n";

const char kRepoShowUsage[] =
    "usage: cidx repo show [-h] [--db PATH] NAME\n";

const char kRepoShowHelp[] =
    "usage: cidx repo show [-h] [--db PATH] NAME\n"
    "\n"
    "positional arguments:\n"
    "  NAME        repository name\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --db PATH   index database (default: the standard cache index)\n";

const char kRepoAddCloneUsage[] =
    "usage: cidx repo add-clone [-h] [--label LABEL] [--db PATH] NAME PATH\n";

const char kRepoAddCloneHelp[] =
    "usage: cidx repo add-clone [-h] [--label LABEL] [--db PATH] NAME PATH\n"
    "\n"
    "positional arguments:\n"
    "  NAME           repository name\n"
    "  PATH           checkout/worktree directory\n"
    "\n"
    "options:\n"
    "  -h, --help     show this help message and exit\n"
    "  --label LABEL  optional label (e.g. branch/worktree name)\n"
    "  --db PATH      index database (default: the standard cache index)\n";

const char kRepoSwitchUsage[] =
    "usage: cidx repo switch [-h] [--db PATH] NAME TARGET\n";

const char kRepoSwitchHelp[] =
    "usage: cidx repo switch [-h] [--db PATH] NAME TARGET\n"
    "\n"
    "positional arguments:\n"
    "  NAME        repository name\n"
    "  TARGET      clone path or label\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --db PATH   index database (default: the standard cache index)\n";

const char kRepoRealiasUsage[] =
    "usage: cidx repo realias [-h] [--db PATH] [COMPONENT]\n";

const char kRepoRealiasHelp[] =
    "usage: cidx repo realias [-h] [--db PATH] [COMPONENT]\n"
    "\n"
    "positional arguments:\n"
    "  COMPONENT   restrict to one component (default: all files)\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --db PATH   index database (default: the standard cache index)\n";

const char kRepoRmUsage[] =
    "usage: cidx repo rm [-h] [--delete-components] [--db PATH] NAME\n";

const char kRepoRmHelp[] =
    "usage: cidx repo rm [-h] [--delete-components] [--db PATH] NAME\n"
    "\n"
    "positional arguments:\n"
    "  NAME                 repository name\n"
    "\n"
    "options:\n"
    "  -h, --help           show this help message and exit\n"
    "  --delete-components  also delete the grouped components and their indexed\n"
    "                       symbols\n"
    "  --db PATH            index database (default: the standard cache index)\n";

const char kDirUsage[] =
    "usage: cidx dir [-h] {list,ls,rm} ...\n";

const char kDirHelp[] =
    "usage: cidx dir [-h] {list,ls,rm} ...\n"
    "\n"
    "positional arguments:\n"
    "  {list,ls,rm}\n"
    "    list (ls)   list directories (all, or one component's)\n"
    "    rm          delete a directory, its files, and their symbols\n"
    "\n"
    "options:\n"
    "  -h, --help    show this help message and exit\n";

const char kDirListUsage[] =
    "usage: cidx dir list [-h] [--component NAME] [pattern]\n";

const char kDirListHelp[] =
    "usage: cidx dir list [-h] [--component NAME] [pattern]\n"
    "\n"
    "positional arguments:\n"
    "  pattern               optional free-text fuzzy filter: characters must\n"
    "                        appear in order, e.g. 'shp' matches shapes.c\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --component NAME, -c NAME\n"
    "                        restrict to this component\n";

const char kDirRmUsage[] =
    "usage: cidx dir rm [-h] (--id ID | --path PATH) [--component NAME] [--dry-run]\n";

const char kDirRmHelp[] =
    "usage: cidx dir rm [-h] (--id ID | --path PATH) [--component NAME] [--dry-run]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --id ID               directory id\n"
    "  --path PATH           directory path\n"
    "  --component NAME, -c NAME\n"
    "                        restrict the match to this component\n"
    "  --dry-run             preview the matches without deleting anything\n";

const char kFileUsage[] =
    "usage: cidx file [-h] {list,ls,show,flags,set,rm} ...\n";

const char kFileHelp[] =
    "usage: cidx file [-h] {list,ls,show,flags,set,rm} ...\n"
    "\n"
    "positional arguments:\n"
    "  {list,ls,show,flags,set,rm}\n"
    "    list (ls)           list files for a component or a directory in it\n"
    "    show                show full details of one file\n"
    "    flags               inspect or edit one file's stored compile flags\n"
    "    set                 set a mutable file attribute (e.g. pending status)\n"
    "    rm                  delete a file and its symbols\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n";

const char kFileListUsage[] =
    "usage: cidx file list [-h] [--component NAME] [--dir PATH]\n"
    "                      [--indexed | --pending]\n"
    "                      [pattern]\n";

const char kFileListHelp[] =
    "usage: cidx file list [-h] [--component NAME] [--dir PATH]\n"
    "                      [--indexed | --pending]\n"
    "                      [pattern]\n"
    "\n"
    "positional arguments:\n"
    "  pattern               optional free-text fuzzy filter: characters must\n"
    "                        appear in order, e.g. 'shp' matches shapes.c\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --component NAME, -c NAME\n"
    "                        restrict to this component\n"
    "  --dir PATH, -d PATH   directory (relative to the component root) including\n"
    "                        its subtree; needs --component\n"
    "  --indexed             only files already indexed\n"
    "  --pending             only files not yet indexed\n";

const char kFileShowUsage[] =
    "usage: cidx file show [-h] [--component NAME] file\n";

const char kFileShowHelp[] =
    "usage: cidx file show [-h] [--component NAME] file\n"
    "\n"
    "positional arguments:\n"
    "  file                  numeric id (first column of 'file list') or a path;\n"
    "                        relative paths resolve against the --component root\n"
    "                        (else the current directory)\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --component NAME, -c NAME\n"
    "                        component root for resolving a relative path\n";

const char kFileFlagsUsage[] =
    "usage: cidx file flags [-h] [--db PATH] COMPONENT://PATH ...\n";

const char kFileFlagsHelp[] =
    "usage: cidx file flags [-h] [--db PATH] COMPONENT://PATH ...\n"
    "\n"
    "positional arguments:\n"
    "  COMPONENT://PATH  file address, e.g. 'mylib://src/foo.c'\n"
    "  OP                -set-flag FLAG | -unset-flag FLAG | -import-args JSON |\n"
    "                    -dump-args (default when omitted)\n"
    "\n"
    "options:\n"
    "  -h, --help        show this help message and exit\n"
    "  --db PATH         operate on this index DB (default: the standard index)\n";

const char kFileSetUsage[] =
    "usage: cidx file set [-h] [--component NAME] [--file REL_PATH] [--db PATH]\n"
    "                     [--dry-run]\n"
    "                     FIELD=VALUE [FIELD=VALUE ...]\n";

const char kFileSetHelp[] =
    "usage: cidx file set [-h] [--component NAME] [--file REL_PATH] [--db PATH]\n"
    "                     [--dry-run]\n"
    "                     FIELD=VALUE [FIELD=VALUE ...]\n"
    "\n"
    "positional arguments:\n"
    "  FIELD=VALUE           attribute assignment, e.g. 'pending=False' (fields:\n"
    "                        pending, indexed)\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --component NAME, -c NAME\n"
    "                        restrict to this component's files\n"
    "  --file REL_PATH       restrict to one file (path relative to component root)\n"
    "  --db PATH             operate on this index DB (default: the standard index)\n"
    "  --dry-run             preview the matches without changing anything\n";

const char kFileRmUsage[] =
    "usage: cidx file rm [-h] (--id ID | --name NAME | --path PATH)\n"
    "                    [--component NAME] [--dry-run]\n";

const char kFileRmHelp[] =
    "usage: cidx file rm [-h] (--id ID | --name NAME | --path PATH)\n"
    "                    [--component NAME] [--dry-run]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --id ID               file id\n"
    "  --name NAME           file basename\n"
    "  --path PATH           file path\n"
    "  --component NAME, -c NAME\n"
    "                        restrict the match to this component\n"
    "  --dry-run             preview the matches without deleting anything\n";

const char kSymbolUsage[] =
    "usage: cidx symbol [-h] {list,ls,show,rm} ...\n";

const char kSymbolHelp[] =
    "usage: cidx symbol [-h] {list,ls,show,rm} ...\n"
    "\n"
    "positional arguments:\n"
    "  {list,ls,show,rm}\n"
    "    list (ls)        list symbols for a component, directory, or file\n"
    "    show             show full details of one symbol\n"
    "    rm               delete a symbol\n"
    "\n"
    "options:\n"
    "  -h, --help         show this help message and exit\n";

const char kSymbolListUsage[] =
    "usage: cidx symbol list [-h] [--component NAME] [--dir PATH] [--file FILE]\n"
    "                        [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                        [--limit N]\n"
    "                        [pattern]\n";

const char kSymbolListHelp[] =
    "usage: cidx symbol list [-h] [--component NAME] [--dir PATH] [--file FILE]\n"
    "                        [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                        [--limit N]\n"
    "                        [pattern]\n"
    "\n"
    "positional arguments:\n"
    "  pattern               optional free-text fuzzy filter: characters must\n"
    "                        appear in order, e.g. 'shp' matches shapes.c (matched\n"
    "                        against the qualified name)\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --component NAME, -c NAME\n"
    "                        restrict to this component\n"
    "  --dir PATH, -d PATH   directory (relative to the component root) including\n"
    "                        its subtree; needs --component\n"
    "  --file FILE, -f FILE  one file; relative paths resolve against the\n"
    "                        --component root (else the current directory)\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict to one symbol kind\n"
    "  --limit N             show at most N matches (0 = all; default 50)\n";

const char kSymbolShowUsage[] =
    "usage: cidx symbol show [-h] symbol\n";

const char kSymbolShowHelp[] =
    "usage: cidx symbol show [-h] symbol\n"
    "\n"
    "positional arguments:\n"
    "  symbol      numeric id (first column of 'search') or a clang USR; USRs\n"
    "              contain $ and * so single-quote them in the shell\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n";

const char kSymbolRmUsage[] =
    "usage: cidx symbol rm [-h] (--id ID | --name NAME | --usr USR)\n"
    "                      [--component NAME] [--dry-run]\n";

const char kSymbolRmHelp[] =
    "usage: cidx symbol rm [-h] (--id ID | --name NAME | --usr USR)\n"
    "                      [--component NAME] [--dry-run]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --id ID               symbol id\n"
    "  --name NAME           symbol spelling\n"
    "  --usr USR             clang USR\n"
    "  --component NAME, -c NAME\n"
    "                        restrict the match to this component\n"
    "  --dry-run             preview the matches without deleting anything\n";

const char kGraphUsage[] =
    "usage: cidx graph [-h]\n"
    "                  {callers,callees,refs,neighbors,walk,path,hierarchy,dispatch,redefined,definitions}\n"
    "                  ...\n";

const char kGraphHelp[] =
    "usage: cidx graph [-h]\n"
    "                  {callers,callees,refs,neighbors,walk,path,hierarchy,dispatch,redefined,definitions}\n"
    "                  ...\n"
    "\n"
    "positional arguments:\n"
    "  {callers,callees,refs,neighbors,walk,path,hierarchy,dispatch,redefined,definitions}\n"
    "    callers             functions that call the symbol\n"
    "    callees             functions the symbol calls\n"
    "    refs                incoming references (calls + uses) to the symbol\n"
    "    neighbors           one-hop typed neighbors\n"
    "    walk                bounded BFS over typed edges\n"
    "    path                shortest path between two symbols, or none\n"
    "    hierarchy           class bases, subclasses, and members\n"
    "    dispatch            run-time targets of a virtual-method call\n"
    "    redefined           symbols defined in more than one backend (multi_def>1)\n"
    "    definitions         each backend body of a symbol + its possible-call fan-\n"
    "                        out\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n";

const char kGraphCallersUsage[] =
    "usage: cidx graph callers [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                          [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                          [--first] [--db PATH] [--json] [--limit N]\n"
    "                          [--direct-only]\n";

const char kGraphCallersHelp[] =
    "usage: cidx graph callers [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                          [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                          [--first] [--db PATH] [--json] [--limit N]\n"
    "                          [--direct-only]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --usr USR             exact clang USR\n"
    "  --id N                numeric symbol id\n"
    "  --name FUZZY          fuzzy qualified-name match ('conf::set')\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict a --name match to one symbol kind\n"
    "  --first               if --name is ambiguous, take the closest match\n"
    "  --db PATH             index database to query (default: the standard cache\n"
    "                        index)\n"
    "  --json                emit stable machine-readable JSON\n"
    "  --limit N             cap the number of results (default 50)\n"
    "  --direct-only         only literal incoming calls (exclude virtual-dispatch\n"
    "                        callers reached via materialised dispatch_calls edges,\n"
    "                        which are on by default)\n";

const char kGraphCalleesUsage[] =
    "usage: cidx graph callees [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                          [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                          [--first] [--db PATH] [--json] [--limit N]\n"
    "                          [--direct-only]\n";

const char kGraphCalleesHelp[] =
    "usage: cidx graph callees [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                          [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                          [--first] [--db PATH] [--json] [--limit N]\n"
    "                          [--direct-only]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --usr USR             exact clang USR\n"
    "  --id N                numeric symbol id\n"
    "  --name FUZZY          fuzzy qualified-name match ('conf::set')\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict a --name match to one symbol kind\n"
    "  --first               if --name is ambiguous, take the closest match\n"
    "  --db PATH             index database to query (default: the standard cache\n"
    "                        index)\n"
    "  --json                emit stable machine-readable JSON\n"
    "  --limit N             cap the number of results (default 50)\n"
    "  --direct-only         only literal outgoing calls (exclude virtual-dispatch\n"
    "                        targets reached via materialised dispatch_calls edges,\n"
    "                        which are on by default)\n";

const char kGraphRefsUsage[] =
    "usage: cidx graph refs [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                       [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                       [--first] [--db PATH] [--json] [--limit N]\n";

const char kGraphRefsHelp[] =
    "usage: cidx graph refs [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                       [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                       [--first] [--db PATH] [--json] [--limit N]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --usr USR             exact clang USR\n"
    "  --id N                numeric symbol id\n"
    "  --name FUZZY          fuzzy qualified-name match ('conf::set')\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict a --name match to one symbol kind\n"
    "  --first               if --name is ambiguous, take the closest match\n"
    "  --db PATH             index database to query (default: the standard cache\n"
    "                        index)\n"
    "  --json                emit stable machine-readable JSON\n"
    "  --limit N             cap the number of results (default 50)\n";

const char kGraphNeighborsUsage[] =
    "usage: cidx graph neighbors [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                            [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                            [--first] [--db PATH] [--json] [--limit N]\n"
    "                            [--edge KINDS] [--direction {in,out}]\n";

const char kGraphNeighborsHelp[] =
    "usage: cidx graph neighbors [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                            [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                            [--first] [--db PATH] [--json] [--limit N]\n"
    "                            [--edge KINDS] [--direction {in,out}]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --usr USR             exact clang USR\n"
    "  --id N                numeric symbol id\n"
    "  --name FUZZY          fuzzy qualified-name match ('conf::set')\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict a --name match to one symbol kind\n"
    "  --first               if --name is ambiguous, take the closest match\n"
    "  --db PATH             index database to query (default: the standard cache\n"
    "                        index)\n"
    "  --json                emit stable machine-readable JSON\n"
    "  --limit N             cap the number of results (default 50)\n"
    "  --edge KINDS          comma-separated edge kinds (calls, construct-copy,\n"
    "                        construct-heap, construct-move, construct-temp,\n"
    "                        construct-value, contains, destroy, dispatch_calls,\n"
    "                        factory-construct, field_of, friend, inherits,\n"
    "                        instantiates, method_of, overrides, specializes, uses)\n"
    "                        (default: all)\n"
    "  --direction {in,out}  edge direction (default out)\n";

const char kGraphWalkUsage[] =
    "usage: cidx graph walk [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                       [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                       [--first] [--db PATH] [--json] [--limit N]\n"
    "                       [--edge KINDS] [--direction {in,out}] [--depth N]\n";

const char kGraphWalkHelp[] =
    "usage: cidx graph walk [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                       [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                       [--first] [--db PATH] [--json] [--limit N]\n"
    "                       [--edge KINDS] [--direction {in,out}] [--depth N]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --usr USR             exact clang USR\n"
    "  --id N                numeric symbol id\n"
    "  --name FUZZY          fuzzy qualified-name match ('conf::set')\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict a --name match to one symbol kind\n"
    "  --first               if --name is ambiguous, take the closest match\n"
    "  --db PATH             index database to query (default: the standard cache\n"
    "                        index)\n"
    "  --json                emit stable machine-readable JSON\n"
    "  --limit N             cap the number of results (default 50)\n"
    "  --edge KINDS          comma-separated edge kinds (calls, construct-copy,\n"
    "                        construct-heap, construct-move, construct-temp,\n"
    "                        construct-value, contains, destroy, dispatch_calls,\n"
    "                        factory-construct, field_of, friend, inherits,\n"
    "                        instantiates, method_of, overrides, specializes, uses)\n"
    "                        (default: calls)\n"
    "  --direction {in,out}  edge direction (default out)\n"
    "  --depth N             max BFS depth (default 3)\n";

const char kGraphPathUsage[] =
    "usage: cidx graph path [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                       [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                       [--first] [--db PATH] [--json] [--limit N]\n"
    "                       (--to-usr USR | --to-id N | --to-name FUZZY)\n"
    "                       [--to-kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                       [--edge KINDS] [--direction {in,out}] [--depth N]\n";

const char kGraphPathHelp[] =
    "usage: cidx graph path [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                       [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                       [--first] [--db PATH] [--json] [--limit N]\n"
    "                       (--to-usr USR | --to-id N | --to-name FUZZY)\n"
    "                       [--to-kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                       [--edge KINDS] [--direction {in,out}] [--depth N]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --usr USR             exact clang USR\n"
    "  --id N                numeric symbol id\n"
    "  --name FUZZY          fuzzy qualified-name match ('conf::set')\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict a --name match to one symbol kind\n"
    "  --first               if --name is ambiguous, take the closest match\n"
    "  --db PATH             index database to query (default: the standard cache\n"
    "                        index)\n"
    "  --json                emit stable machine-readable JSON\n"
    "  --limit N             cap the number of results (default 50)\n"
    "  --to-usr USR          destination by USR\n"
    "  --to-id N             destination by id\n"
    "  --to-name FUZZY       destination by name\n"
    "  --to-kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict a --to-name match to one symbol kind\n"
    "  --edge KINDS          comma-separated edge kinds (calls, construct-copy,\n"
    "                        construct-heap, construct-move, construct-temp,\n"
    "                        construct-value, contains, destroy, dispatch_calls,\n"
    "                        factory-construct, field_of, friend, inherits,\n"
    "                        instantiates, method_of, overrides, specializes, uses)\n"
    "                        (default: calls)\n"
    "  --direction {in,out}  edge direction (default out)\n"
    "  --depth N             max search depth (default 8)\n";

const char kGraphHierarchyUsage[] =
    "usage: cidx graph hierarchy [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                            [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                            [--first] [--db PATH] [--json] [--limit N]\n"
    "                            [--transitive]\n"
    "                            [--access {public,protected,private,all}]\n";

const char kGraphHierarchyHelp[] =
    "usage: cidx graph hierarchy [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                            [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                            [--first] [--db PATH] [--json] [--limit N]\n"
    "                            [--transitive]\n"
    "                            [--access {public,protected,private,all}]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --usr USR             exact clang USR\n"
    "  --id N                numeric symbol id\n"
    "  --name FUZZY          fuzzy qualified-name match ('conf::set')\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict a --name match to one symbol kind\n"
    "  --first               if --name is ambiguous, take the closest match\n"
    "  --db PATH             index database to query (default: the standard cache\n"
    "                        index)\n"
    "  --json                emit stable machine-readable JSON\n"
    "  --limit N             cap the number of results (default 50)\n"
    "  --transitive          walk the whole inheritance tree, not just direct edges\n"
    "  --access {public,protected,private,all}\n"
    "                        filter members by C++ access specifier (default all)\n";

const char kGraphDispatchUsage[] =
    "usage: cidx graph dispatch [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                           [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                           [--first] [--db PATH] [--json] [--limit N]\n";

const char kGraphDispatchHelp[] =
    "usage: cidx graph dispatch [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                           [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                           [--first] [--db PATH] [--json] [--limit N]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --usr USR             exact clang USR\n"
    "  --id N                numeric symbol id\n"
    "  --name FUZZY          fuzzy qualified-name match ('conf::set')\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict a --name match to one symbol kind\n"
    "  --first               if --name is ambiguous, take the closest match\n"
    "  --db PATH             index database to query (default: the standard cache\n"
    "                        index)\n"
    "  --json                emit stable machine-readable JSON\n"
    "  --limit N             cap the number of results (default 50)\n";

const char kGraphRedefinedUsage[] =
    "usage: cidx graph redefined [-h] [--db PATH] [--json] [--limit N]\n";

const char kGraphRedefinedHelp[] =
    "usage: cidx graph redefined [-h] [--db PATH] [--json] [--limit N]\n"
    "\n"
    "options:\n"
    "  -h, --help  show this help message and exit\n"
    "  --db PATH   index database to query (default: the standard cache index)\n"
    "  --json      emit stable machine-readable JSON\n"
    "  --limit N   cap the number of results (default 200)\n";

const char kGraphDefinitionsUsage[] =
    "usage: cidx graph definitions [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                              [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                              [--first] [--db PATH] [--json] [--limit N]\n"
    "                              [--direct-only]\n";

const char kGraphDefinitionsHelp[] =
    "usage: cidx graph definitions [-h] (--usr USR | --id N | --name FUZZY)\n"
    "                              [--kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}]\n"
    "                              [--first] [--db PATH] [--json] [--limit N]\n"
    "                              [--direct-only]\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --usr USR             exact clang USR\n"
    "  --id N                numeric symbol id\n"
    "  --name FUZZY          fuzzy qualified-name match ('conf::set')\n"
    "  --kind {class,class-template,constructor,destructor,enum,enum-constant,function,function-template,macro,member,method,namespace,struct,type-alias,typedef,union,variable}\n"
    "                        restrict a --name match to one symbol kind\n"
    "  --first               if --name is ambiguous, take the closest match\n"
    "  --db PATH             index database to query (default: the standard cache\n"
    "                        index)\n"
    "  --json                emit stable machine-readable JSON\n"
    "  --limit N             cap the number of results (default 50)\n"
    "  --direct-only         list the backend bodies only (omit the possible-call\n"
    "                        fan-out, which is shown by default)\n";

// ---------------------------------------------------------------------------
// Choice sets
// ---------------------------------------------------------------------------

const std::vector<std::string> kComponentKinds = {"repo", "external"};
const std::vector<std::string> kSymbolKinds = {
    "class",   "class-template", "constructor", "destructor",
    "enum",    "enum-constant",  "function",    "function-template",
    "macro",   "member",         "method",      "namespace",
    "struct",  "type-alias",     "typedef",     "union",
    "variable"};
const std::vector<std::string> kCommands = {
    "init",      "import", "index", "resolve", "search",
    "analyze",   "db",     "component", "repo", "dir",
    "file",      "symbol", "graph"};
const std::vector<std::string> kDbWhats = {"migrate", "verify"};
const std::vector<std::string> kComponentWhats = {
    "add", "list", "ls", "show", "set-version", "compile-commands", "rm"};
const std::vector<std::string> kRepoWhats = {
    "list", "ls", "show", "add-clone", "switch", "realias", "rm"};
const std::vector<std::string> kDirWhats = {"list", "ls", "rm"};
const std::vector<std::string> kFileWhats = {"list", "ls", "show",
                                             "flags", "set", "rm"};
const std::vector<std::string> kSymbolWhats = {"list", "ls", "show", "rm"};
const std::vector<std::string> kGraphWhats = {
    "callers",   "callees",  "refs",      "neighbors",   "walk",
    "path",      "hierarchy", "dispatch", "redefined",   "definitions"};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

enum class ValueKind { kNone, kString, kInt };

struct OptSpec {
  const char *name;     // long option, "--limit"
  char short_opt;       // 'c' or '\0'
  ValueKind value;      // kNone = store_true flag
  const char *err_name; // argparse's name in messages: "--component/-c"
  const std::vector<std::string> *choices = nullptr;
  int mutex = 0;            // mutually-exclusive group id; 0 = none
  bool repeatable = false;  // argparse action="append" (collect every value)
};

struct Spec {
  const char *prog;  // "cidx search"
  const char *usage; // usage block, trailing '\n'
  const char *help;  // full help text, trailing '\n'
  std::vector<OptSpec> opts;
  std::vector<const char *> positionals; // fixed positionals, in order
  bool rest = false;                     // collect surplus (index FILE...)
  // names reported by the required check, in argparse action-add order
  std::vector<const char *> required;
  // mutex group ids that argparse marks required: when none of a group's
  // members is seen, argparse fails with "one of the arguments ... is required"
  std::vector<int> required_mutex = {};
  // argparse.REMAINDER: once the fixed positionals are filled, every remaining
  // token (option-looking ones included) is captured verbatim into st.rest.
  bool remainder = false;
};

struct ParseState {
  std::map<std::string, std::string> values; // long name -> raw value
  std::map<std::string, std::vector<std::string>> multi; // append-action values
  std::map<std::string, bool> flags;
  std::vector<std::string> positionals;
  std::vector<std::string> rest;
  bool help = false;
};

[[noreturn]] void fail(const char *usage, const std::string &prog,
                       const std::string &msg) {
  throw UsageError(std::string(usage) + prog + ": error: " + msg + "\n", 2);
}

[[noreturn]] void fail(const Spec &spec, const std::string &msg) {
  fail(spec.usage, spec.prog, msg);
}

std::string join(const std::vector<std::string> &parts,
                 const std::string &sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out += sep;
    }
    out += parts[i];
  }
  return out;
}

// argparse's _negative_number_matcher: ^-\d+$|^-\d*\.\d+$ — such tokens are
// treated as values/positionals because no option string looks like one.
bool is_negative_number(const std::string &tok) {
  if (tok.size() < 2 || tok[0] != '-') {
    return false;
  }
  std::size_t i = 1;
  std::size_t digits = 0;
  while (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i]))) {
    ++i;
    ++digits;
  }
  if (i == tok.size()) {
    return digits > 0; // ^-\d+$
  }
  if (tok[i] != '.') {
    return false;
  }
  ++i; // ^-\d*\.\d+$
  digits = 0;
  while (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i]))) {
    ++i;
    ++digits;
  }
  return i == tok.size() && digits > 0;
}

bool is_option_token(const std::string &tok) {
  return tok.size() > 1 && tok[0] == '-' && !is_negative_number(tok);
}

// Python int(str): surrounding whitespace stripped, optional sign, digits.
bool parse_py_int(const std::string &raw, long &out) {
  std::size_t b = 0;
  std::size_t e = raw.size();
  while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) {
    ++b;
  }
  while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) {
    --e;
  }
  if (b == e) {
    return false;
  }
  bool neg = false;
  if (raw[b] == '+' || raw[b] == '-') {
    neg = raw[b] == '-';
    ++b;
  }
  if (b == e) {
    return false;
  }
  // Saturate positive accumulation at INT_MAX to avoid signed-overflow UB.
  // Huge positive limit → INT_MAX → "show all" semantics are preserved.
  constexpr long kPosMax = static_cast<long>(std::numeric_limits<int>::max());
  long val = 0;
  bool saturated = false;
  for (std::size_t i = b; i < e; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(raw[i]))) {
      return false;
    }
    const long digit = raw[i] - '0';
    if (!neg && val > (kPosMax - digit) / 10) {
      // Would overflow INT_MAX: consume and validate remaining digits, then cap.
      while (++i < e) {
        if (!std::isdigit(static_cast<unsigned char>(raw[i]))) {
          return false;
        }
      }
      saturated = true;
      val = kPosMax;
      break;
    }
    val = val * 10 + digit;
  }
  (void)saturated;
  out = neg ? -val : val;
  return true;
}

const OptSpec *find_long(const Spec &spec, const std::string &name) {
  // Exact match first (fast path; also avoids ambiguity when the token IS a
  // full option name, e.g. "--name" vs "--name-with-suffix").
  for (const OptSpec &o : spec.opts) {
    if (name == o.name) {
      return &o;
    }
  }
  // Unambiguous prefix match — mirrors Python argparse allow_abbrev=True.
  // Return the unique option whose long name starts with `name`; if zero or
  // two-or-more options match, return nullptr (treated as unrecognized).
  const std::size_t nlen = name.size();
  const OptSpec *match = nullptr;
  for (const OptSpec &o : spec.opts) {
    // o.name is const char*; use strncmp to check if it starts with `name`.
    if (std::strncmp(o.name, name.c_str(), nlen) == 0) {
      if (match != nullptr) {
        // Ambiguous: more than one option matches the prefix → unrecognized.
        return nullptr;
      }
      match = &o;
    }
  }
  return match;
}

const OptSpec *find_short(const Spec &spec, char c) {
  for (const OptSpec &o : spec.opts) {
    if (o.short_opt != '\0' && o.short_opt == c) {
      return &o;
    }
  }
  return nullptr;
}

// Validate at encounter time (argparse converts/checks per consumed
// argument, in scan order).
void check_value(const Spec &spec, const OptSpec &opt, const std::string &val) {
  if (opt.choices != nullptr) {
    for (const std::string &c : *opt.choices) {
      if (val == c) {
        return;
      }
    }
    fail(spec, "argument " + std::string(opt.err_name) + ": invalid choice: '" +
                   val + "' (choose from " + join(*opt.choices, ", ") + ")");
  }
  if (opt.value == ValueKind::kInt) {
    long parsed = 0;
    if (!parse_py_int(val, parsed)) {
      fail(spec, "argument " + std::string(opt.err_name) +
                     ": invalid int value: '" + val + "'");
    }
  }
}

// Parse tokens[i..] against a leaf spec. Unknown options / surplus
// positionals are appended to `extras` (reported by the caller as the TOP
// parser's "unrecognized arguments" — argparse parse_known_args semantics,
// fired only after subparser-level errors had their chance).
ParseState parse_leaf(const Spec &spec, const std::vector<std::string> &tokens,
                      std::size_t i, std::vector<std::string> &extras) {
  ParseState st;
  std::map<int, const char *> mutex_seen;
  bool only_positionals = false;
  const std::size_t n = tokens.size();
  while (i < n) {
    const std::string &tok = tokens[i];
    // argparse.REMAINDER: after the fixed positionals are filled, capture the
    // rest verbatim (including -flag-looking tokens) without option parsing.
    if (spec.remainder && st.positionals.size() >= spec.positionals.size()) {
      st.rest.push_back(tok);
      ++i;
      continue;
    }
    if (!only_positionals && tok == "--") {
      only_positionals = true;
      ++i;
      continue;
    }
    if (!only_positionals && is_option_token(tok)) {
      if (tok == "-h" || tok == "--help") {
        st.help = true;
        return st;
      }
      const OptSpec *opt = nullptr;
      std::string inline_val;
      bool has_inline = false;
      if (tok.starts_with("--")) {
        std::string name = tok;
        const std::size_t eq = tok.find('=');
        if (eq != std::string::npos) {
          name = tok.substr(0, eq);
          inline_val = tok.substr(eq + 1);
          has_inline = true;
        }
        opt = find_long(spec, name); // exact match only — no abbreviation (D6)
      } else {
        opt = find_short(spec, tok[1]);
        if (opt != nullptr && tok.size() > 2) { // glued short value: -cNAME
          inline_val = tok.substr(2);
          has_inline = true;
        }
      }
      if (opt == nullptr) {
        extras.push_back(tok);
        ++i;
        continue;
      }
      if (opt->mutex != 0) {
        auto seen = mutex_seen.find(opt->mutex);
        if (seen != mutex_seen.end() && seen->second != opt->err_name) {
          fail(spec, "argument " + std::string(opt->err_name) +
                         ": not allowed with argument " + seen->second);
        }
        mutex_seen[opt->mutex] = opt->err_name;
      }
      if (opt->value == ValueKind::kNone) {
        if (has_inline) {
          fail(spec, "argument " + std::string(opt->err_name) +
                         ": ignored explicit argument '" + inline_val + "'");
        }
        st.flags[opt->name] = true;
        ++i;
        continue;
      }
      std::string val;
      if (has_inline) {
        val = inline_val;
      } else {
        if (i + 1 >= n || is_option_token(tokens[i + 1])) {
          fail(spec, "argument " + std::string(opt->err_name) +
                         ": expected one argument");
        }
        val = tokens[++i];
      }
      check_value(spec, *opt, val);
      st.values[opt->name] = val; // repeated flags: last one wins (argparse)
      if (opt->repeatable) {
        st.multi[opt->name].push_back(val); // append action: keep every value
      }
      ++i;
      continue;
    }
    // positional
    if (st.positionals.size() < spec.positionals.size()) {
      st.positionals.push_back(tok);
    } else if (spec.rest) {
      st.rest.push_back(tok);
    } else {
      extras.push_back(tok);
    }
    ++i;
  }
  // Required check (argparse: after the scan, before the caller's
  // unrecognized-arguments check — verified against the Python tool).
  std::vector<std::string> missing;
  for (const char *name : spec.required) {
    if (name[0] == '-') {
      if (st.values.find(name) == st.values.end()) {
        missing.push_back(name);
      }
    } else {
      std::size_t pos_index = 0;
      for (std::size_t p = 0; p < spec.positionals.size(); ++p) {
        if (std::string(spec.positionals[p]) == name) {
          pos_index = p;
          break;
        }
      }
      if (pos_index >= st.positionals.size()) {
        missing.push_back(name);
      }
    }
  }
  if (!missing.empty()) {
    fail(spec, "the following arguments are required: " + join(missing, ", "));
  }
  // Required mutually-exclusive groups: argparse fails when none of the
  // group's members was supplied. Members are listed in spec.opts order.
  for (const int grp : spec.required_mutex) {
    std::vector<std::string> members;
    bool seen = false;
    for (const OptSpec &o : spec.opts) {
      if (o.mutex == grp) {
        members.emplace_back(o.name);
        if (st.values.find(o.name) != st.values.end() ||
            st.flags.find(o.name) != st.flags.end()) {
          seen = true;
        }
      }
    }
    if (!seen) {
      fail(spec, "one of the arguments " + join(members, " ") + " is required");
    }
  }
  return st;
}

std::optional<std::string> opt_value(const ParseState &st, const char *name) {
  auto it = st.values.find(name);
  if (it == st.values.end()) {
    return std::nullopt;
  }
  return it->second;
}

int int_value(const ParseState &st, const char *name, int def) {
  auto it = st.values.find(name);
  if (it == st.values.end()) {
    return def;
  }
  long parsed = 0;
  parse_py_int(it->second, parsed); // validated at encounter; positive saturated at INT_MAX
  // Clamp to [INT_MIN, INT_MAX] to make static_cast safe.
  if (parsed > static_cast<long>(std::numeric_limits<int>::max())) {
    parsed = static_cast<long>(std::numeric_limits<int>::max());
  } else if (parsed < static_cast<long>(std::numeric_limits<int>::min())) {
    parsed = static_cast<long>(std::numeric_limits<int>::min());
  }
  return static_cast<int>(parsed);
}

// Scan for a sub-command token (the top parser and the show/list parsers all
// do this): options before it go to extras, -h shows this level's help.
struct CommandScan {
  std::optional<std::string> command;
  std::size_t next = 0;
  bool help = false;
  bool version = false;
};

CommandScan scan_command(const std::vector<std::string> &tokens, std::size_t i,
                         std::vector<std::string> &extras,
                         bool allow_version = false) {
  CommandScan out;
  const std::size_t n = tokens.size();
  while (i < n) {
    const std::string &tok = tokens[i];
    if (tok == "-h" || tok == "--help") {
      out.help = true;
      out.next = i + 1;
      return out;
    }
    // argparse's version action fires the instant `--version` is consumed
    // (before the required-subcommand check). Only the top parser registers
    // it, so sub-scans (show/list/delete) leave it to the extras path.
    if (allow_version && tok == "--version") {
      out.version = true;
      out.next = i + 1;
      return out;
    }
    if (is_option_token(tok)) {
      extras.push_back(tok);
      ++i;
      continue;
    }
    out.command = tok;
    out.next = i + 1;
    return out;
  }
  out.next = n;
  return out;
}

bool contains(const std::vector<std::string> &v, const std::string &s) {
  for (const std::string &e : v) {
    if (e == s) {
      return true;
    }
  }
  return false;
}


// -- leaf specs --------------------------------------------------------------

const Spec kInitSpec = {
    "cidx init",
    kInitUsage,
    kInitHelp,
    {
        {"--force", '\0', ValueKind::kNone, "--force", nullptr, 0},
    },
    {},
    false,
    {},
};

const Spec kImportSpec = {
    "cidx import",
    kImportUsage,
    kImportHelp,
    {
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
        {"--name", '\0', ValueKind::kString, "--name", nullptr, 0},
        {"--repo", '\0', ValueKind::kString, "--repo", nullptr, 0},
        {"--force", '\0', ValueKind::kNone, "--force", nullptr, 0},
        // v0.6.0: aliasing
        {"--no-alias", '\0', ValueKind::kNone, "--no-alias", nullptr, 0},
    },
    {},
    false,
    {"--db"},
};

const Spec kIndexSpec = {
    "cidx index",
    kIndexUsage,
    kIndexHelp,
    {
        {"--source", '\0', ValueKind::kString, "--source", nullptr, 0},
        {"--no-graph", '\0', ValueKind::kNone, "--no-graph", nullptr, 0},
        // v14: portable-paths
        {"--no-autoderive-labels", '\0', ValueKind::kNone,
         "--no-autoderive-labels", nullptr, 0},
    },
    {},
    true, // files: nargs="*"
    {},
};

const Spec kResolveSpec = {
    "cidx resolve",
    kResolveUsage,
    kResolveHelp,
    {},
    {},
    false,
    {},
};

const Spec kSearchSpec = {
    "cidx search",
    kSearchUsage,
    kSearchHelp,
    {
        {"--kind", '\0', ValueKind::kString, "--kind", &kSymbolKinds, 0},
        {"--limit", '\0', ValueKind::kInt, "--limit", nullptr, 0},
    },
    {"pattern"},
    false,
    {"pattern"},
};

const Spec kAnalyzeSpec = {
    "cidx analyze",
    kAnalyzeUsage,
    kAnalyzeHelp,
    {
        {"--rule", '\0', ValueKind::kString, "--rule", nullptr, 0},
        {"--rules-file", '\0', ValueKind::kString, "--rules-file", nullptr, 0},
        {"--list", '\0', ValueKind::kNone, "--list", nullptr, 0},
        {"--export-facts", '\0', ValueKind::kString, "--export-facts", nullptr,
         0},
        {"--jobs", '\0', ValueKind::kInt, "--jobs", nullptr, 0},
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {}, // no positionals
    false,
    {}, // nothing required (the handler enforces exactly-one-mode)
};

// -- db leaf specs (v0.53.0 regrouping) ---------------------------------------

const Spec kDbMigrateSpec = {
    "cidx db migrate",
    kDbMigrateUsage,
    kDbMigrateHelp,
    {
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {},
    false, // no positional
    {},
};

const Spec kDbVerifySpec = {
    "cidx db verify",
    kDbVerifyUsage,
    kDbVerifyHelp,
    {
        {"--component", 'c', ValueKind::kString, "--component/-c", nullptr, 0},
        {"--all", '\0', ValueKind::kNone, "--all", nullptr, 0},
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {}, // no positionals
    false,
    {}, // nothing required
};

// -- component leaf specs ------------------------------------------------------

const Spec kComponentAddSpec = {
    "cidx component add",
    kComponentAddUsage,
    kComponentAddHelp,
    {
        {"--path", '\0', ValueKind::kString, "--path", nullptr, 0},
        {"--name", '\0', ValueKind::kString, "--name", nullptr, 0},
        {"--repo", '\0', ValueKind::kString, "--repo", nullptr, 0},
        {"--kind", '\0', ValueKind::kString, "--kind", &kComponentKinds, 0},
        {"--no-git", '\0', ValueKind::kNone, "--no-git", nullptr, 0},
        // v14: portable-paths
        {"--version", '\0', ValueKind::kString, "--version", nullptr, 0},
        {"--no-detect-version", '\0', ValueKind::kNone, "--no-detect-version",
         nullptr, 0},
    },
    {},
    false,
    {"--path"},
};

const Spec kComponentListSpec = {
    "cidx component list",
    kComponentListUsage,
    kComponentListHelp,
    {
        {"--kind", '\0', ValueKind::kString, "--kind", &kComponentKinds, 0},
    },
    {"pattern"},
    false,
    {},
};

const Spec kComponentShowSpec = {
    "cidx component show",
    kComponentShowUsage,
    kComponentShowHelp,
    {
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {"NAME"},
    false,
    {"NAME"},
};

const Spec kComponentSetVersionSpec = {
    "cidx component set-version",
    kComponentSetVersionUsage,
    kComponentSetVersionHelp,
    {
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {"NAME", "VERSION"}, // VERSION is optional (nargs=?)
    true,                // rest: captures any surplus (including the optional VERSION)
    {"NAME"},
};

const Spec kComponentCcSpec = {
    "cidx component compile-commands",
    kComponentCcUsage,
    kComponentCcHelp,
    {
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {"COMPONENT"}, // the required component positional
    false,
    {"COMPONENT"}, // required
};

const Spec kComponentRmSpec = {
    "cidx component rm",
    kComponentRmUsage,
    kComponentRmHelp,
    {
        {"--id", '\0', ValueKind::kInt, "--id", nullptr, 1},
        {"--name", '\0', ValueKind::kString, "--name", nullptr, 1},
        {"--path", '\0', ValueKind::kString, "--path", nullptr, 1},
        {"--dry-run", '\0', ValueKind::kNone, "--dry-run", nullptr, 0},
    },
    {},
    false,
    {},
    {1},
};

// -- repo leaf specs (v23) ---------------------------------------------------

const Spec kRepoListSpec = {
    "cidx repo list",
    kRepoListUsage,
    kRepoListHelp,
    {
        {"--kind", '\0', ValueKind::kString, "--kind", &kComponentKinds, 0},
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {"pattern"}, // optional (not in required set)
    false,
    {},
};

const Spec kRepoShowSpec = {
    "cidx repo show",
    kRepoShowUsage,
    kRepoShowHelp,
    {
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {"NAME"},
    false,
    {"NAME"},
};

const Spec kRepoAddCloneSpec = {
    "cidx repo add-clone",
    kRepoAddCloneUsage,
    kRepoAddCloneHelp,
    {
        {"--label", '\0', ValueKind::kString, "--label", nullptr, 0},
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {"NAME", "PATH"},
    false,
    {"NAME", "PATH"},
};

const Spec kRepoSwitchSpec = {
    "cidx repo switch",
    kRepoSwitchUsage,
    kRepoSwitchHelp,
    {
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {"NAME", "TARGET"},
    false,
    {"NAME", "TARGET"},
};

const Spec kRepoRealiasSpec = {
    "cidx repo realias",
    kRepoRealiasUsage,
    kRepoRealiasHelp,
    {
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {}, // COMPONENT is optional, collected as rest
    true,  // rest=true: nargs="?" — collect surplus tokens as optional positional
    {},    // nothing required
};

const Spec kRepoRmSpec = {
    "cidx repo rm",
    kRepoRmUsage,
    kRepoRmHelp,
    {
        {"--delete-components", '\0', ValueKind::kNone, "--delete-components",
         nullptr, 0},
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {"NAME"},
    false,
    {"NAME"},
};

// -- dir leaf specs ------------------------------------------------------------

const Spec kDirListSpec = {
    "cidx dir list",
    kDirListUsage,
    kDirListHelp,
    {
        {"--component", 'c', ValueKind::kString, "--component/-c", nullptr, 0},
    },
    {"pattern"},
    false,
    {},
};

const Spec kDirRmSpec = {
    "cidx dir rm",
    kDirRmUsage,
    kDirRmHelp,
    {
        {"--id", '\0', ValueKind::kInt, "--id", nullptr, 1},
        {"--path", '\0', ValueKind::kString, "--path", nullptr, 1},
        {"--component", 'c', ValueKind::kString, "--component/-c", nullptr, 0},
        {"--dry-run", '\0', ValueKind::kNone, "--dry-run", nullptr, 0},
    },
    {},
    false,
    {},
    {1},
};

// -- file leaf specs -----------------------------------------------------------

const Spec kFileListSpec = {
    "cidx file list",
    kFileListUsage,
    kFileListHelp,
    {
        {"--component", 'c', ValueKind::kString, "--component/-c", nullptr, 0},
        {"--dir", 'd', ValueKind::kString, "--dir/-d", nullptr, 0},
        {"--indexed", '\0', ValueKind::kNone, "--indexed", nullptr, 1},
        {"--pending", '\0', ValueKind::kNone, "--pending", nullptr, 1},
    },
    {"pattern"},
    false,
    {},
};

const Spec kFileShowSpec = {
    "cidx file show",
    kFileShowUsage,
    kFileShowHelp,
    {
        {"--component", 'c', ValueKind::kString, "--component/-c", nullptr, 0},
    },
    {"file"},
    false,
    {"file"},
};

const Spec kFileFlagsSpec = {
    "cidx file flags",
    kFileFlagsUsage,
    kFileFlagsHelp,
    {
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
    },
    {"COMPONENT://PATH"}, // the required target positional
    false,                // surplus is captured by the REMAINDER tail, not rest
    {"COMPONENT://PATH"}, // required
    {},                   // no required-mutex groups
    true,                 // nargs=REMAINDER: capture OP ... verbatim
};

const Spec kFileSetSpec = {
    "cidx file set",
    kFileSetUsage,
    kFileSetHelp,
    {
        {"--component", 'c', ValueKind::kString, "--component/-c", nullptr, 0},
        {"--file", '\0', ValueKind::kString, "--file", nullptr, 0},
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
        {"--dry-run", '\0', ValueKind::kNone, "--dry-run", nullptr, 0},
    },
    {"FIELD=VALUE"}, // first positional; name reported by the required check
    true,            // nargs="+": collect surplus FIELD=VALUE tokens
    {"FIELD=VALUE"}, // required
};

const Spec kFileRmSpec = {
    "cidx file rm",
    kFileRmUsage,
    kFileRmHelp,
    {
        {"--id", '\0', ValueKind::kInt, "--id", nullptr, 1},
        {"--name", '\0', ValueKind::kString, "--name", nullptr, 1},
        {"--path", '\0', ValueKind::kString, "--path", nullptr, 1},
        {"--component", 'c', ValueKind::kString, "--component/-c", nullptr, 0},
        {"--dry-run", '\0', ValueKind::kNone, "--dry-run", nullptr, 0},
    },
    {},
    false,
    {},
    {1},
};

// -- symbol leaf specs ---------------------------------------------------------

const Spec kSymbolListSpec = {
    "cidx symbol list",
    kSymbolListUsage,
    kSymbolListHelp,
    {
        {"--component", 'c', ValueKind::kString, "--component/-c", nullptr, 0},
        {"--dir", 'd', ValueKind::kString, "--dir/-d", nullptr, 0},
        {"--file", 'f', ValueKind::kString, "--file/-f", nullptr, 0},
        {"--kind", '\0', ValueKind::kString, "--kind", &kSymbolKinds, 0},
        {"--limit", '\0', ValueKind::kInt, "--limit", nullptr, 0},
    },
    {"pattern"},
    false,
    {},
};

const Spec kSymbolShowSpec = {
    "cidx symbol show", kSymbolShowUsage,
    kSymbolShowHelp,    {},
    {"symbol"},         false,
    {"symbol"},
};

const Spec kSymbolRmSpec = {
    "cidx symbol rm",
    kSymbolRmUsage,
    kSymbolRmHelp,
    {
        {"--id", '\0', ValueKind::kInt, "--id", nullptr, 1},
        {"--name", '\0', ValueKind::kString, "--name", nullptr, 1},
        {"--usr", '\0', ValueKind::kString, "--usr", nullptr, 1},
        {"--component", 'c', ValueKind::kString, "--component/-c", nullptr, 0},
        {"--dry-run", '\0', ValueKind::kNone, "--dry-run", nullptr, 0},
    },
    {},
    false,
    {},
    {1},
};

// -- graph leaf specs (M6) ---------------------------------------------------
// mutex group 1: --usr|--id|--name (required selector)
// mutex group 2: --to-usr|--to-id|--to-name (required path destination)
#define GRAPH_SELECTOR_OPTS                                                    \
  {"--usr", '\0', ValueKind::kString, "--usr", nullptr, 1},                   \
      {"--id", '\0', ValueKind::kInt, "--id", nullptr, 1},                    \
      {"--name", '\0', ValueKind::kString, "--name", nullptr, 1},             \
      {"--kind", '\0', ValueKind::kString, "--kind", &kSymbolKinds, 0},       \
      {"--first", '\0', ValueKind::kNone, "--first", nullptr, 0},             \
      {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},                 \
      {"--json", '\0', ValueKind::kNone, "--json", nullptr, 0},               \
  {"--limit", '\0', ValueKind::kInt, "--limit", nullptr, 0}

const std::vector<std::string> kDirectionChoices = {"in", "out"};
const std::vector<std::string> kAccessChoices = {
    "public", "protected", "private", "all"};

const Spec kGraphCallersSpec = {
    "cidx graph callers",
    kGraphCallersUsage,
    kGraphCallersHelp,
    {GRAPH_SELECTOR_OPTS,
     {"--direct-only", '\0', ValueKind::kNone, "--direct-only", nullptr, 0}},
    {},
    false,
    {},
    {1}, // required mutex: one of --usr|--id|--name
};

const Spec kGraphCalleesSpec = {
    "cidx graph callees",
    kGraphCalleesUsage,
    kGraphCalleesHelp,
    {GRAPH_SELECTOR_OPTS,
     {"--direct-only", '\0', ValueKind::kNone, "--direct-only", nullptr, 0}},
    {},
    false,
    {},
    {1},
};

const Spec kGraphRefsSpec = {
    "cidx graph refs",
    kGraphRefsUsage,
    kGraphRefsHelp,
    {GRAPH_SELECTOR_OPTS},
    {},
    false,
    {},
    {1},
};

const Spec kGraphNeighborsSpec = {
    "cidx graph neighbors",
    kGraphNeighborsUsage,
    kGraphNeighborsHelp,
    {
        GRAPH_SELECTOR_OPTS,
        {"--edge", '\0', ValueKind::kString, "--edge", nullptr, 0},
        {"--direction", '\0', ValueKind::kString, "--direction",
         &kDirectionChoices, 0},
    },
    {},
    false,
    {},
    {1},
};

const Spec kGraphWalkSpec = {
    "cidx graph walk",
    kGraphWalkUsage,
    kGraphWalkHelp,
    {
        GRAPH_SELECTOR_OPTS,
        {"--edge", '\0', ValueKind::kString, "--edge", nullptr, 0},
        {"--direction", '\0', ValueKind::kString, "--direction",
         &kDirectionChoices, 0},
        {"--depth", '\0', ValueKind::kInt, "--depth", nullptr, 0},
    },
    {},
    false,
    {},
    {1},
};

const Spec kGraphPathSpec = {
    "cidx graph path",
    kGraphPathUsage,
    kGraphPathHelp,
    {
        GRAPH_SELECTOR_OPTS,
        {"--to-usr", '\0', ValueKind::kString, "--to-usr", nullptr, 2},
        {"--to-id", '\0', ValueKind::kInt, "--to-id", nullptr, 2},
        {"--to-name", '\0', ValueKind::kString, "--to-name", nullptr, 2},
        {"--to-kind", '\0', ValueKind::kString, "--to-kind", &kSymbolKinds, 0},
        {"--edge", '\0', ValueKind::kString, "--edge", nullptr, 0},
        {"--direction", '\0', ValueKind::kString, "--direction",
         &kDirectionChoices, 0},
        {"--depth", '\0', ValueKind::kInt, "--depth", nullptr, 0},
    },
    {},
    false,
    {},
    {1, 2}, // both selector and destination required
};

const Spec kGraphHierarchySpec = {
    "cidx graph hierarchy",
    kGraphHierarchyUsage,
    kGraphHierarchyHelp,
    {
        GRAPH_SELECTOR_OPTS,
        {"--transitive", '\0', ValueKind::kNone, "--transitive", nullptr, 0},
        {"--access", '\0', ValueKind::kString, "--access", &kAccessChoices, 0},
    },
    {},
    false,
    {},
    {1},
};

const Spec kGraphDispatchSpec = {
    "cidx graph dispatch",
    kGraphDispatchUsage,
    kGraphDispatchHelp,
    {GRAPH_SELECTOR_OPTS},
    {},
    false,
    {},
    {1},
};

// v27: definitions (like dispatch/callers: symbol selector + --direct-only).
const Spec kGraphDefinitionsSpec = {
    "cidx graph definitions",
    kGraphDefinitionsUsage,
    kGraphDefinitionsHelp,
    {GRAPH_SELECTOR_OPTS,
     {"--direct-only", '\0', ValueKind::kNone, "--direct-only", nullptr, 0}},
    {},
    false,
    {},
    {1},
};

// v27: redefined (no symbol selector -- only --db/--json/--limit).
const Spec kGraphRedefinedSpec = {
    "cidx graph redefined",
    kGraphRedefinedUsage,
    kGraphRedefinedHelp,
    {
        {"--db", '\0', ValueKind::kString, "--db", nullptr, 0},
        {"--json", '\0', ValueKind::kNone, "--json", nullptr, 0},
        {"--limit", '\0', ValueKind::kInt, "--limit", nullptr, 0},
    },
    {},
    false,
    {},
    {},
};

} // namespace

namespace {

bool parse_graph_command(const std::vector<std::string> &argv, std::size_t i,
                         std::vector<std::string> &extras, ParsedArgs &pa) {
  auto fill_selector = [&](const ParseState &st) {
    pa.usr = opt_value(st, "--usr");
    if (const auto v = opt_value(st, "--id")) {
      long parsed = 0;
      parse_py_int(*v, parsed);
      pa.graph_id = static_cast<int64_t>(parsed);
    }
    pa.name = opt_value(st, "--name");
    pa.kind = opt_value(st, "--kind");
    pa.first = st.flags.count("--first") != 0;
    pa.index_db = opt_value(st, "--db");
    pa.graph_json = st.flags.count("--json") != 0;
    pa.graph_limit = int_value(st, "--limit", 50);
  };

  CommandScan what = scan_command(argv, i, extras);
  if (what.help) {
    pa.help_text = kGraphHelp;
    return true;
  }
  if (!what.command) {
    fail(kGraphUsage, "cidx graph",
         "the following arguments are required: what");
  }
  if (!contains(kGraphWhats, *what.command)) {
    fail(kGraphUsage, "cidx graph",
         "argument what: invalid choice: '" + *what.command +
             "' (choose from " + join(kGraphWhats, ", ") + ")");
  }
  pa.what = *what.command;

  if (pa.what == "callers") {
    ParseState st = parse_leaf(kGraphCallersSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kGraphCallersHelp;
      return true;
    }
    fill_selector(st);
    pa.direct_only = st.flags.count("--direct-only") != 0;
  } else if (pa.what == "callees") {
    ParseState st = parse_leaf(kGraphCalleesSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kGraphCalleesHelp;
      return true;
    }
    fill_selector(st);
    pa.direct_only = st.flags.count("--direct-only") != 0;
  } else if (pa.what == "refs") {
    ParseState st = parse_leaf(kGraphRefsSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kGraphRefsHelp;
      return true;
    }
    fill_selector(st);
  } else if (pa.what == "neighbors") {
    ParseState st = parse_leaf(kGraphNeighborsSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kGraphNeighborsHelp;
      return true;
    }
    fill_selector(st);
    pa.edge = opt_value(st, "--edge");
    pa.direction = opt_value(st, "--direction").value_or("out");
  } else if (pa.what == "walk") {
    ParseState st = parse_leaf(kGraphWalkSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kGraphWalkHelp;
      return true;
    }
    fill_selector(st);
    pa.edge = opt_value(st, "--edge");
    pa.direction = opt_value(st, "--direction").value_or("out");
    pa.graph_depth = int_value(st, "--depth", 3);
  } else if (pa.what == "path") {
    ParseState st = parse_leaf(kGraphPathSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kGraphPathHelp;
      return true;
    }
    fill_selector(st);
    pa.to_usr = opt_value(st, "--to-usr");
    if (const auto v = opt_value(st, "--to-id")) {
      long parsed = 0;
      parse_py_int(*v, parsed);
      pa.to_id = static_cast<int64_t>(parsed);
    }
    pa.to_name = opt_value(st, "--to-name");
    pa.to_kind = opt_value(st, "--to-kind");
    pa.edge = opt_value(st, "--edge");
    pa.direction = opt_value(st, "--direction").value_or("out");
    pa.graph_depth = int_value(st, "--depth", 8);
  } else if (pa.what == "hierarchy") {
    ParseState st = parse_leaf(kGraphHierarchySpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kGraphHierarchyHelp;
      return true;
    }
    fill_selector(st);
    pa.transitive = st.flags.count("--transitive") != 0;
    pa.access = opt_value(st, "--access").value_or("all");
  } else if (pa.what == "dispatch") {
    ParseState st = parse_leaf(kGraphDispatchSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kGraphDispatchHelp;
      return true;
    }
    fill_selector(st);
  } else if (pa.what == "definitions") {
    ParseState st = parse_leaf(kGraphDefinitionsSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kGraphDefinitionsHelp;
      return true;
    }
    fill_selector(st);
    pa.direct_only = st.flags.count("--direct-only") != 0;
  } else {
    ParseState st = parse_leaf(kGraphRedefinedSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kGraphRedefinedHelp;
      return true;
    }
    pa.index_db = opt_value(st, "--db");
    pa.graph_json = st.flags.count("--json") != 0;
    pa.graph_limit = int_value(st, "--limit", 200);
  }

  if (pa.index_db) {
    pa.index_db = pathutil::abspath(pathutil::expanduser(*pa.index_db));
  }
  return false;
}

bool parse_db_command(const std::vector<std::string> &argv, std::size_t i,
                      std::vector<std::string> &extras, ParsedArgs &pa) {
  CommandScan what = scan_command(argv, i, extras);
  if (what.help) {
    pa.help_text = kDbHelp;
    return true;
  }
  if (!what.command) {
    fail(kDbUsage, "cidx db", "the following arguments are required: what");
  }
  if (!contains(kDbWhats, *what.command)) {
    fail(kDbUsage, "cidx db",
         "argument what: invalid choice: '" + *what.command +
             "' (choose from " + join(kDbWhats, ", ") + ")");
  }
  if (*what.command == "migrate") {
    pa.command = "migrate";
    ParseState st = parse_leaf(kDbMigrateSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kDbMigrateHelp;
      return true;
    }
    pa.index_db = opt_value(st, "--db");
  } else {
    pa.command = "verify";
    ParseState st = parse_leaf(kDbVerifySpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kDbVerifyHelp;
      return true;
    }
    pa.component = opt_value(st, "--component");
    pa.all = st.flags.count("--all") != 0;
    pa.index_db = opt_value(st, "--db");
  }
  return false;
}

bool parse_component_command(const std::vector<std::string> &argv,
                             std::size_t i,
                             std::vector<std::string> &extras,
                             ParsedArgs &pa) {
  CommandScan what = scan_command(argv, i, extras);
  if (what.help) {
    pa.help_text = kComponentHelp;
    return true;
  }
  if (!what.command) {
    fail(kComponentUsage, "cidx component",
         "the following arguments are required: what");
  }
  if (!contains(kComponentWhats, *what.command)) {
    fail(kComponentUsage, "cidx component",
         "argument what: invalid choice: '" + *what.command +
             "' (choose from " + join(kComponentWhats, ", ") + ")");
  }
  const std::string &sub = *what.command;
  if (sub == "add") {
    pa.command = "add-source";
    ParseState st = parse_leaf(kComponentAddSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kComponentAddHelp;
      return true;
    }
    pa.path = st.values["--path"];
    pa.name = opt_value(st, "--name");
    pa.repo = opt_value(st, "--repo");
    pa.kind = opt_value(st, "--kind");
    if (!pa.kind) {
      pa.kind = "repo"; // argparse default
    }
    pa.no_git = st.flags.count("--no-git") != 0;
    pa.version_str = opt_value(st, "--version");
    pa.no_detect_version = st.flags.count("--no-detect-version") != 0;
  } else if (sub == "list" || sub == "ls") {
    pa.command = "list";
    pa.what = "components";
    ParseState st = parse_leaf(kComponentListSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kComponentListHelp;
      return true;
    }
    if (!st.positionals.empty()) {
      pa.pattern = st.positionals[0];
    }
    pa.kind = opt_value(st, "--kind");
  } else if (sub == "show") {
    pa.command = "component";
    pa.what = "show";
    ParseState st = parse_leaf(kComponentShowSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kComponentShowHelp;
      return true;
    }
    pa.name = st.positionals[0];
    pa.index_db = opt_value(st, "--db");
  } else if (sub == "set-version") {
    pa.command = "component";
    pa.what = "set-version";
    ParseState st =
        parse_leaf(kComponentSetVersionSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kComponentSetVersionHelp;
      return true;
    }
    pa.name = st.positionals[0];
    if (st.positionals.size() >= 2) {
      pa.version_str = st.positionals[1];
    } else if (!st.rest.empty()) {
      pa.version_str = st.rest[0];
    }
    pa.index_db = opt_value(st, "--db");
  } else if (sub == "compile-commands") {
    pa.command = "dump-compile-commands";
    ParseState st = parse_leaf(kComponentCcSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kComponentCcHelp;
      return true;
    }
    pa.component = st.positionals[0];
    pa.index_db = opt_value(st, "--db");
  } else { // rm
    pa.command = "delete";
    pa.what = "component";
    ParseState st = parse_leaf(kComponentRmSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kComponentRmHelp;
      return true;
    }
    if (const std::optional<std::string> id = opt_value(st, "--id")) {
      long parsed = 0;
      parse_py_int(*id, parsed); // validated at encounter time
      pa.del_id = static_cast<int64_t>(parsed);
    }
    pa.name = opt_value(st, "--name");
    pa.del_path = opt_value(st, "--path");
    pa.dry_run = st.flags.count("--dry-run") != 0;
  }
  return false;
}

bool parse_repo_command(const std::vector<std::string> &argv, std::size_t i,
                        std::vector<std::string> &extras, ParsedArgs &pa) {
  CommandScan what = scan_command(argv, i, extras);
  if (what.help) {
    pa.help_text = kRepoHelp;
    return true;
  }
  if (!what.command) {
    fail(kRepoUsage, "cidx repo", "the following arguments are required: what");
  }
  if (!contains(kRepoWhats, *what.command)) {
    fail(kRepoUsage, "cidx repo",
         "argument what: invalid choice: '" + *what.command +
             "' (choose from " + join(kRepoWhats, ", ") + ")");
  }
  pa.what = *what.command;
  if (pa.what == "list" || pa.what == "ls") {
    ParseState st = parse_leaf(kRepoListSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kRepoListHelp;
      return true;
    }
    if (!st.positionals.empty()) {
      pa.pattern = st.positionals[0];
    }
    pa.kind = opt_value(st, "--kind");
    pa.index_db = opt_value(st, "--db");
  } else if (pa.what == "show") {
    ParseState st = parse_leaf(kRepoShowSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kRepoShowHelp;
      return true;
    }
    pa.name = st.positionals[0];
    pa.index_db = opt_value(st, "--db");
  } else if (pa.what == "add-clone") {
    ParseState st = parse_leaf(kRepoAddCloneSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kRepoAddCloneHelp;
      return true;
    }
    pa.name = st.positionals[0];
    pa.path = st.positionals[1];
    pa.repo_label = opt_value(st, "--label");
    pa.index_db = opt_value(st, "--db");
  } else if (pa.what == "switch") {
    ParseState st = parse_leaf(kRepoSwitchSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kRepoSwitchHelp;
      return true;
    }
    pa.name = st.positionals[0];
    pa.target = st.positionals[1];
    pa.index_db = opt_value(st, "--db");
  } else if (pa.what == "realias") {
    pa.command = "realias";
    ParseState st = parse_leaf(kRepoRealiasSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kRepoRealiasHelp;
      return true;
    }
    // Optional COMPONENT positional collected via rest.
    if (!st.rest.empty()) {
      pa.component = st.rest[0];
    }
    pa.index_db = opt_value(st, "--db");
  } else {
    ParseState st = parse_leaf(kRepoRmSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kRepoRmHelp;
      return true;
    }
    pa.name = st.positionals[0];
    pa.delete_components = st.flags.count("--delete-components") != 0;
    pa.index_db = opt_value(st, "--db");
  }
  return false;
}

bool parse_dir_command(const std::vector<std::string> &argv, std::size_t i,
                       std::vector<std::string> &extras, ParsedArgs &pa) {
  CommandScan what = scan_command(argv, i, extras);
  if (what.help) {
    pa.help_text = kDirHelp;
    return true;
  }
  if (!what.command) {
    fail(kDirUsage, "cidx dir", "the following arguments are required: what");
  }
  if (!contains(kDirWhats, *what.command)) {
    fail(kDirUsage, "cidx dir",
         "argument what: invalid choice: '" + *what.command +
             "' (choose from " + join(kDirWhats, ", ") + ")");
  }
  const std::string &sub = *what.command;
  if (sub == "list" || sub == "ls") {
    pa.command = "list";
    pa.what = "dirs";
    ParseState st = parse_leaf(kDirListSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kDirListHelp;
      return true;
    }
    if (!st.positionals.empty()) {
      pa.pattern = st.positionals[0];
    }
    pa.component = opt_value(st, "--component");
  } else { // rm
    pa.command = "delete";
    pa.what = "dir";
    ParseState st = parse_leaf(kDirRmSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kDirRmHelp;
      return true;
    }
    if (const std::optional<std::string> id = opt_value(st, "--id")) {
      long parsed = 0;
      parse_py_int(*id, parsed);
      pa.del_id = static_cast<int64_t>(parsed);
    }
    pa.del_path = opt_value(st, "--path");
    pa.component = opt_value(st, "--component");
    pa.dry_run = st.flags.count("--dry-run") != 0;
  }
  return false;
}

bool parse_file_command(const std::vector<std::string> &argv, std::size_t i,
                        std::vector<std::string> &extras, ParsedArgs &pa) {
  CommandScan what = scan_command(argv, i, extras);
  if (what.help) {
    pa.help_text = kFileHelp;
    return true;
  }
  if (!what.command) {
    fail(kFileUsage, "cidx file", "the following arguments are required: what");
  }
  if (!contains(kFileWhats, *what.command)) {
    fail(kFileUsage, "cidx file",
         "argument what: invalid choice: '" + *what.command +
             "' (choose from " + join(kFileWhats, ", ") + ")");
  }
  const std::string &sub = *what.command;
  if (sub == "list" || sub == "ls") {
    pa.command = "list";
    pa.what = "files";
    ParseState st = parse_leaf(kFileListSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kFileListHelp;
      return true;
    }
    if (!st.positionals.empty()) {
      pa.pattern = st.positionals[0];
    }
    pa.component = opt_value(st, "--component");
    pa.dir = opt_value(st, "--dir");
    pa.indexed = st.flags.count("--indexed") != 0;
    pa.pending = st.flags.count("--pending") != 0;
  } else if (sub == "show") {
    pa.command = "show";
    pa.what = "file";
    ParseState st = parse_leaf(kFileShowSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kFileShowHelp;
      return true;
    }
    pa.file = st.positionals[0];
    pa.component = opt_value(st, "--component");
  } else if (sub == "flags") {
    pa.command = "file";
    ParseState st = parse_leaf(kFileFlagsSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kFileFlagsHelp;
      return true;
    }
    pa.target = st.positionals[0];
    pa.op = st.rest; // REMAINDER tail: the operation + its args, verbatim
    pa.index_db = opt_value(st, "--db");
  } else if (sub == "set") {
    pa.command = "set";
    ParseState st = parse_leaf(kFileSetSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kFileSetHelp;
      return true;
    }
    // assignment = the fixed positional plus any surplus (nargs="+").
    pa.assignment.push_back(st.positionals[0]);
    for (const std::string &tok : st.rest) {
      pa.assignment.push_back(tok);
    }
    pa.component = opt_value(st, "--component");
    pa.file_filter = opt_value(st, "--file");
    pa.index_db = opt_value(st, "--db");
    pa.dry_run = st.flags.count("--dry-run") != 0;
  } else { // rm
    pa.command = "delete";
    pa.what = "file";
    ParseState st = parse_leaf(kFileRmSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kFileRmHelp;
      return true;
    }
    if (const std::optional<std::string> id = opt_value(st, "--id")) {
      long parsed = 0;
      parse_py_int(*id, parsed);
      pa.del_id = static_cast<int64_t>(parsed);
    }
    pa.name = opt_value(st, "--name");
    pa.del_path = opt_value(st, "--path");
    pa.component = opt_value(st, "--component");
    pa.dry_run = st.flags.count("--dry-run") != 0;
  }
  return false;
}

bool parse_symbol_command(const std::vector<std::string> &argv, std::size_t i,
                          std::vector<std::string> &extras, ParsedArgs &pa) {
  CommandScan what = scan_command(argv, i, extras);
  if (what.help) {
    pa.help_text = kSymbolHelp;
    return true;
  }
  if (!what.command) {
    fail(kSymbolUsage, "cidx symbol",
         "the following arguments are required: what");
  }
  if (!contains(kSymbolWhats, *what.command)) {
    fail(kSymbolUsage, "cidx symbol",
         "argument what: invalid choice: '" + *what.command +
             "' (choose from " + join(kSymbolWhats, ", ") + ")");
  }
  const std::string &sub = *what.command;
  if (sub == "list" || sub == "ls") {
    pa.command = "list";
    pa.what = "symbols";
    ParseState st = parse_leaf(kSymbolListSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kSymbolListHelp;
      return true;
    }
    if (!st.positionals.empty()) {
      pa.pattern = st.positionals[0];
    }
    pa.component = opt_value(st, "--component");
    pa.dir = opt_value(st, "--dir");
    pa.file_filter = opt_value(st, "--file");
    pa.kind = opt_value(st, "--kind");
    pa.limit = int_value(st, "--limit", 50);
  } else if (sub == "show") {
    pa.command = "show";
    pa.what = "symbol";
    ParseState st = parse_leaf(kSymbolShowSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kSymbolShowHelp;
      return true;
    }
    pa.symbol = st.positionals[0];
  } else { // rm
    pa.command = "delete";
    pa.what = "symbol";
    ParseState st = parse_leaf(kSymbolRmSpec, argv, what.next, extras);
    if (st.help) {
      pa.help_text = kSymbolRmHelp;
      return true;
    }
    if (const std::optional<std::string> id = opt_value(st, "--id")) {
      long parsed = 0;
      parse_py_int(*id, parsed);
      pa.del_id = static_cast<int64_t>(parsed);
    }
    pa.name = opt_value(st, "--name");
    pa.usr = opt_value(st, "--usr");
    pa.component = opt_value(st, "--component");
    pa.dry_run = st.flags.count("--dry-run") != 0;
  }
  return false;
}

} // namespace

ParsedArgs parse_args(const std::vector<std::string> &argv) {
  std::vector<std::string> extras;
  ParsedArgs pa;

  CommandScan top = scan_command(argv, 0, extras, /*allow_version=*/true);
  if (top.help) {
    pa.help_text = kTopHelp;
    return pa;
  }
  if (top.version) {
    pa.version = true;
    return pa;
  }
  if (!top.command) {
    fail(kTopUsage, "cidx", "the following arguments are required: command");
  }
  if (!contains(kCommands, *top.command)) {
    fail(kTopUsage, "cidx",
         "argument command: invalid choice: '" + *top.command +
             "' (choose from " + join(kCommands, ", ") + ")");
  }
  pa.command = *top.command;
  std::size_t i = top.next;

  if (pa.command == "init") {
    ParseState st = parse_leaf(kInitSpec, argv, i, extras);
    if (st.help) {
      pa.help_text = kInitHelp;
      return pa;
    }
    pa.force = st.flags.count("--force") != 0;
  } else if (pa.command == "import") {
    ParseState st = parse_leaf(kImportSpec, argv, i, extras);
    if (st.help) {
      pa.help_text = kImportHelp;
      return pa;
    }
    pa.db = st.values["--db"];
    pa.name = opt_value(st, "--name");
    pa.repo = opt_value(st, "--repo");
    pa.force = st.flags.count("--force") != 0;
    pa.no_alias = st.flags.count("--no-alias") != 0;
  } else if (pa.command == "index") {
    ParseState st = parse_leaf(kIndexSpec, argv, i, extras);
    if (st.help) {
      pa.help_text = kIndexHelp;
      return pa;
    }
    pa.files = st.rest;
    pa.source = opt_value(st, "--source");
    pa.no_graph = st.flags.count("--no-graph") != 0;
    pa.no_autoderive_labels = st.flags.count("--no-autoderive-labels") != 0;
  } else if (pa.command == "resolve") {
    ParseState st = parse_leaf(kResolveSpec, argv, i, extras);
    if (st.help) {
      pa.help_text = kResolveHelp;
      return pa;
    }
  } else if (pa.command == "search") {
    ParseState st = parse_leaf(kSearchSpec, argv, i, extras);
    if (st.help) {
      pa.help_text = kSearchHelp;
      return pa;
    }
    pa.pattern = st.positionals[0];
    pa.kind = opt_value(st, "--kind");
    pa.limit = int_value(st, "--limit", 25);
  } else if (pa.command == "analyze") {
    ParseState st = parse_leaf(kAnalyzeSpec, argv, i, extras);
    if (st.help) {
      pa.help_text = kAnalyzeHelp;
      return pa;
    }
    pa.analyze_rule = opt_value(st, "--rule");
    pa.analyze_rules_file = opt_value(st, "--rules-file");
    pa.analyze_list = st.flags.count("--list") != 0;
    pa.analyze_export = opt_value(st, "--export-facts");
    pa.analyze_jobs = int_value(st, "--jobs", 1);
    pa.index_db = opt_value(st, "--db");
  } else if (pa.command == "db") {
    if (parse_db_command(argv, i, extras, pa)) {
      return pa;
    }
  } else if (pa.command == "component") {
    if (parse_component_command(argv, i, extras, pa)) {
      return pa;
    }
  } else if (pa.command == "repo") {
    if (parse_repo_command(argv, i, extras, pa)) {
      return pa;
    }
  } else if (pa.command == "dir") {
    if (parse_dir_command(argv, i, extras, pa)) {
      return pa;
    }
  } else if (pa.command == "file") {
    if (parse_file_command(argv, i, extras, pa)) {
      return pa;
    }
  } else if (pa.command == "symbol") {
    if (parse_symbol_command(argv, i, extras, pa)) {
      return pa;
    }
  } else if (pa.command == "graph") {
    if (parse_graph_command(argv, i, extras, pa)) {
      return pa;
    }
  }

  // argparse parse_args: anything parse_known_args left over is reported by
  // the TOP parser, after all subparser-level errors had their chance.
  if (!extras.empty()) {
    fail(kTopUsage, "cidx", "unrecognized arguments: " + join(extras, " "));
  }
  return pa;
}

} // namespace cli
} // namespace cidx
