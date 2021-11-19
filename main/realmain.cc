#ifdef SORBET_REALMAIN_MIN
// minimal build to speedup compilation. Remove extra features
#else
#define FULL_BUILD_ONLY(X) X;
#include "core/proto/proto.h" // has to be included first as it violates our poisons
// intentional comment to stop from reformatting
#include "common/statsd/statsd.h"
#include "common/web_tracer_framework/tracing.h"
#include "main/autogen/autogen.h"
#include "main/autogen/autoloader.h"
#include "main/autogen/crc_builder.h"
#include "main/autogen/data/version.h"
#include "main/autogen/dsl_analysis.h"
#include "main/autogen/packages.h"
#include "main/autogen/subclasses.h"
#include "main/lsp/LSPInput.h"
#include "main/lsp/LSPOutput.h"
#include "main/lsp/lsp.h"
#include "main/minimize/minimize.h"
#endif

#include "absl/strings/str_cat.h"
#include "common/FileOps.h"
#include "common/Timer.h"
#include "common/sort.h"
#include "core/Error.h"
#include "core/ErrorQueue.h"
#include "core/Files.h"
#include "core/NullFlusher.h"
#include "core/Unfreeze.h"
#include "core/errors/errors.h"
#include "core/lsp/QueryResponse.h"
#include "core/serialize/serialize.h"
#include "hashing/hashing.h"
#include "main/cache/cache.h"
#include "main/pipeline/pipeline.h"
#include "main/realmain.h"
#include "payload/payload.h"
#include "resolver/resolver.h"
#include "sorbet_version/sorbet_version.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <csignal>
#include <poll.h>

namespace spd = spdlog;

using namespace std;

namespace sorbet::realmain {
shared_ptr<spd::logger> logger;
int returnCode;

shared_ptr<spd::sinks::ansicolor_stderr_sink_mt> make_stderrColorSink() {
    auto color_sink = make_shared<spd::sinks::ansicolor_stderr_sink_mt>();
    color_sink->set_color(spd::level::info, color_sink->white);
    color_sink->set_color(spd::level::debug, color_sink->magenta);
    color_sink->set_level(spd::level::info);
    return color_sink;
}

shared_ptr<spd::sinks::ansicolor_stderr_sink_mt> stderrColorSink = make_stderrColorSink();

/*
 * Workaround https://bugzilla.mindrot.org/show_bug.cgi?id=2863 ; We are
 * commonly run under ssh with a controlmaster, and we write exclusively to
 * STDERR in normal usage. If the client goes away, we can hang forever writing
 * to a full pipe buffer on stderr.
 *
 * Workaround by monitoring for STDOUT to go away and self-HUPing.
 */
void startHUPMonitor() {
    thread monitor([]() {
        struct pollfd pfd;
        setCurrentThreadName("HUPMonitor");
        pfd.fd = 1; // STDOUT
        pfd.events = 0;
        pfd.revents = 0;
        while (true) {
            int rv = poll(&pfd, 1, -1);
            if (rv <= 0) {
                continue;
            }
            if ((pfd.revents & (POLLHUP | POLLERR)) != 0) {
                // STDOUT has gone away; Exit via SIGHUP.
                kill(getpid(), SIGHUP);
            }
        }
    });
    monitor.detach();
}

core::StrictLevel levelMinusOne(core::StrictLevel level) {
    switch (level) {
        case core::StrictLevel::Ignore:
            return core::StrictLevel::None;
        case core::StrictLevel::False:
            return core::StrictLevel::Ignore;
        case core::StrictLevel::True:
            return core::StrictLevel::False;
        case core::StrictLevel::Strict:
            return core::StrictLevel::True;
        case core::StrictLevel::Strong:
            return core::StrictLevel::Strict;
        case core::StrictLevel::Max:
            return core::StrictLevel::Strong;
        default:
            Exception::raise("Should never happen");
    }
}

// Filter levels to a sensible recommendation.
core::StrictLevel levelToRecommendation(core::StrictLevel level) {
    switch (level) {
        case core::StrictLevel::Internal:
        case core::StrictLevel::None:
            Exception::raise("Should never happen");

        case core::StrictLevel::Ignore:
        case core::StrictLevel::False:
        case core::StrictLevel::True:
        case core::StrictLevel::Strict:
            return level;

        case core::StrictLevel::Strong:
        case core::StrictLevel::Max:
            return core::StrictLevel::Strict;

        case core::StrictLevel::Autogenerated:
        case core::StrictLevel::Stdlib:
            Exception::raise("Should never happen");
    }
}

string levelToSigil(core::StrictLevel level) {
    switch (level) {
        case core::StrictLevel::None:
            Exception::raise("Should never happen");
        case core::StrictLevel::Internal:
            Exception::raise("Should never happen");
        case core::StrictLevel::Ignore:
            return "ignore";
        case core::StrictLevel::False:
            return "false";
        case core::StrictLevel::True:
            return "true";
        case core::StrictLevel::Strict:
            return "strict";
        case core::StrictLevel::Strong:
            return "strong";
        case core::StrictLevel::Max:
            Exception::raise("Should never happen");
        case core::StrictLevel::Autogenerated:
            Exception::raise("Should never happen");
        case core::StrictLevel::Stdlib:
            return "__STDLIB_INTERNAL";
    }
}

core::Loc findTyped(unique_ptr<core::GlobalState> &gs, core::FileRef file) {
    auto source = file.data(*gs).source();

    if (file.data(*gs).originalSigil == core::StrictLevel::None) {
        if (source.length() >= 2 && source[0] == '#' && source[1] == '!') {
            int newline = source.find("\n", 0);
            return core::Loc(file, newline + 1, newline + 1);
        }
        return core::Loc(file, 0, 0);
    }
    size_t start = 0;
    start = source.find("typed:", start);
    if (start == string_view::npos) {
        return core::Loc(file, 0, 0);
    }
    while (start >= 0 && source[start] != '#') {
        --start;
    }
    auto end = start;
    while (end < source.size() && source[end] != '\n') {
        ++end;
    }
    if (source[end] == '\n') {
        ++end;
    }
    return core::Loc(file, start, end);
}

#ifndef SORBET_REALMAIN_MIN
struct AutogenResult {
    struct Serialized {
        // Selectively populated based on print options
        string strval;
        string msgpack;
        vector<string> classlist;
        optional<autogen::Subclasses::Map> subclasses;
        optional<UnorderedMap<vector<core::NameRef>, autogen::DSLInfo>> dslInfo;
    };
    CounterState counters;
    vector<pair<int, Serialized>> prints;
    unique_ptr<autogen::DefTree> defTree = make_unique<autogen::DefTree>();
};

void runAutogen(const core::GlobalState &gs, options::Options &opts, const autogen::AutoloaderConfig &autoloaderCfg,
                WorkerPool &workers, vector<ast::ParsedFile> &indexed) {
    Timer timeit(logger, "autogen");

    // extract all the packages we can find. (This ought to be pretty fast: if it's not, then we can move this into the
    // parallel loop below.)
    vector<autogen::Package> packageq;
    for (auto i = 0; i < indexed.size(); ++i) {
        if (indexed[i].file.data(gs).isPackage()) {
            auto &tree = indexed[i];
            core::Context ctx(gs, core::Symbols::root(), tree.file);
            packageq.emplace_back(autogen::Packages::extractPackage(ctx, move(tree)));
        }
    }

    auto resultq = make_shared<BlockingBoundedQueue<AutogenResult>>(indexed.size());
    auto fileq = make_shared<ConcurrentBoundedQueue<int>>(indexed.size());
    vector<AutogenResult::Serialized> merged(indexed.size());
    for (int i = 0; i < indexed.size(); ++i) {
        fileq->push(i, 1);
    }
    auto crcBuilder = autogen::CRCBuilder::create();

    workers.multiplexJob("runAutogen", [&gs, &opts, &indexed, &autoloaderCfg, crcBuilder, fileq, resultq]() {
        AutogenResult out;
        int n = 0;
        int autogenVersion = opts.autogenVersion == 0 ? autogen::AutogenVersion::MAX_VERSION : opts.autogenVersion;
        {
            Timer timeit(logger, "autogenWorker");
            int idx = 0;

            for (auto result = fileq->try_pop(idx); !result.done(); result = fileq->try_pop(idx)) {
                ++n;
                auto &tree = indexed[idx];
                if (tree.file.data(gs).isPackage()) {
                    continue;
                }
                if (autogenVersion < autogen::AutogenVersion::VERSION_INCLUDE_RBI && tree.file.data(gs).isRBI()) {
                    continue;
                }

                core::Context ctx(gs, core::Symbols::root(), tree.file);
                auto pf = autogen::Autogen::generate(ctx, move(tree), *crcBuilder);
                tree = move(pf.tree);

                AutogenResult::Serialized serialized;

                if (opts.print.Autogen.enabled) {
                    Timer timeit(logger, "autogenToString");
                    serialized.strval = pf.toString(ctx, autogenVersion);
                }
                if (opts.print.DSLAnalysis.enabled) {
                    auto &tree2 = indexed[idx];
                    Timer timeit(logger, "dslAnalysisToString");
                    auto daf = autogen::DSLAnalysis::generate(ctx, move(tree2), *crcBuilder);
                    serialized.dslInfo = std::move(daf.dslInfo);
                }
                if (opts.print.AutogenMsgPack.enabled) {
                    Timer timeit(logger, "autogenToMsgpack");
                    serialized.msgpack = pf.toMsgpack(ctx, autogenVersion);
                }

                if (!tree.file.data(gs).isRBI()) {
                    // Exclude RBI files because they are not loadable and should not appear in
                    // auto-loader related output.
                    if (opts.print.AutogenClasslist.enabled) {
                        Timer timeit(logger, "autogenClasslist");
                        serialized.classlist = pf.listAllClasses(ctx);
                    }
                    if (opts.print.AutogenSubclasses.enabled) {
                        Timer timeit(logger, "autogenSubclasses");
                        serialized.subclasses = autogen::Subclasses::listAllSubclasses(
                            ctx, pf, opts.autogenSubclassesAbsoluteIgnorePatterns,
                            opts.autogenSubclassesRelativeIgnorePatterns);
                    }
                    if (opts.print.AutogenAutoloader.enabled) {
                        Timer timeit(logger, "autogenNamedDefs");
                        autogen::DefTreeBuilder::addParsedFileDefinitions(ctx, autoloaderCfg, out.defTree, pf);
                    }
                }

                out.prints.emplace_back(make_pair(idx, serialized));
            }
        }

        out.counters = getAndClearThreadCounters();
        resultq->push(move(out), n);
    });

    autogen::DefTree root;
    AutogenResult out;
    for (auto res = resultq->wait_pop_timed(out, WorkerPool::BLOCK_INTERVAL(), *logger); !res.done();
         res = resultq->wait_pop_timed(out, WorkerPool::BLOCK_INTERVAL(), *logger)) {
        if (!res.gotItem()) {
            continue;
        }
        counterConsume(move(out.counters));
        for (auto &print : out.prints) {
            merged[print.first] = move(print.second);
        }
        if (opts.print.AutogenAutoloader.enabled) {
            Timer timeit(logger, "autogenAutoloaderDefTreeMerge");
            root = autogen::DefTreeBuilder::merge(gs, move(root), move(*out.defTree));
        }
    }

    {
        Timer timeit(logger, "autogenDependencyDBPrint");
        for (auto &elem : merged) {
            if (opts.print.Autogen.enabled) {
                opts.print.Autogen.print(elem.strval);
            }
            if (opts.print.AutogenMsgPack.enabled) {
                opts.print.AutogenMsgPack.print(elem.msgpack);
            }
        }
    }
    if (opts.print.AutogenAutoloader.enabled) {
        {
            Timer timeit(logger, "autogenAutoloaderPrune");
            autogen::DefTreeBuilder::collapseSameFileDefs(gs, autoloaderCfg, root);
        }
        {
            Timer timeit(logger, "autogenAutoloaderWrite");
            autogen::AutoloadWriter::writeAutoloads(gs, workers, autoloaderCfg, opts.print.AutogenAutoloader.outputPath,
                                                    root);
        }
    }

    if (opts.print.AutogenClasslist.enabled) {
        Timer timeit(logger, "autogenClasslistPrint");
        vector<string> mergedClasslist;
        for (auto &el : merged) {
            auto &v = el.classlist;
            mergedClasslist.insert(mergedClasslist.end(), make_move_iterator(v.begin()), make_move_iterator(v.end()));
        }
        fast_sort(mergedClasslist);
        auto last = unique(mergedClasslist.begin(), mergedClasslist.end());
        opts.print.AutogenClasslist.fmt("{}\n", fmt::join(mergedClasslist.begin(), last, "\n"));
    }

    if (opts.print.AutogenSubclasses.enabled) {
        Timer timeit(logger, "autogenSubclassesPrint");

        // Merge the {Parent: Set{Child1, Child2}} maps from each thread
        autogen::Subclasses::Map childMap;
        for (const auto &el : merged) {
            if (!el.subclasses) {
                // File doesn't define any Child < Parent relationships
                continue;
            }

            for (const auto &[parentName, children] : *el.subclasses) {
                if (!parentName.empty()) {
                    childMap[parentName].entries.insert(children.entries.begin(), children.entries.end());
                    childMap[parentName].classKind = children.classKind;
                }
            }
        }

        vector<string> serializedDescendantsMap =
            autogen::Subclasses::genDescendantsMap(childMap, opts.autogenSubclassesParents);

        opts.print.AutogenSubclasses.fmt(
            "{}\n", fmt::join(serializedDescendantsMap.begin(), serializedDescendantsMap.end(), "\n"));
    }

    if (opts.print.DSLAnalysis.enabled) {
        Timer timeit(logger, "autogenDSLAnalysisPrint");

        UnorderedMap<std::vector<core::NameRef>, autogen::DSLInfo> globalDSLInfo;

        for (const auto &el : merged) {
            if (el.dslInfo) {
                for (auto &it : std::move(*el.dslInfo)) {
                    globalDSLInfo.emplace(it.first, it.second);
                }
            }
        }

        const auto &processedGlobalDSLInfo = autogen::mergeAndFilterGlobalDSLInfo(std::move(globalDSLInfo));
        fmt::memory_buffer out;

        int totalMutators = 0;
        int problemMutators = 0;
        for (const auto &it : processedGlobalDSLInfo) {
            if (it.second.model.empty() || it.second.props.empty()) {
                continue;
            }

            totalMutators++;

            if (it.second.problemLocs.empty()) {
                continue;
            }

            problemMutators++;
        }

        fmt::format_to(std::back_inserter(out), "Number of mutators analyzed: {}\n", totalMutators);
        fmt::format_to(std::back_inserter(out), "Number of mutators with problems: {}\n\n", problemMutators);

        for (const auto &it : processedGlobalDSLInfo) {
            if (it.second.model.empty() || it.second.props.empty() || !it.second.problemLocs.empty()) {
                continue;
            }
            autogen::printName(out, it.first, gs);
            it.second.formatString(out, gs);
        }

        opts.print.DSLAnalysis.fmt("{}", to_string(out));
    }

    if (opts.autoloaderConfig.packagedAutoloader) {
        Timer timeit(logger, "autogenPackageAutoloads");
        autogen::AutoloadWriter::writePackageAutoloads(gs, autoloaderCfg, opts.print.AutogenAutoloader.outputPath,
                                                       packageq);
    }
}
#endif

int realmain(int argc, char *argv[]) {
#ifndef SORBET_REALMAIN_MIN
    initializeSymbolizer(argv[0]);
#endif
    returnCode = 0;
    logger = make_shared<spd::logger>("console", stderrColorSink);
    logger->set_level(spd::level::trace); // pass through everything, let the sinks decide
    logger->set_pattern("%v");
    fatalLogger = logger;

    auto typeErrorsConsole = make_shared<spd::logger>("typeDiagnostics", stderrColorSink);
    typeErrorsConsole->set_pattern("%v");

    options::Options opts;
    auto extensionProviders = sorbet::pipeline::semantic_extension::SemanticExtensionProvider::getProviders();
    vector<unique_ptr<sorbet::pipeline::semantic_extension::SemanticExtension>> extensions;
    options::readOptions(opts, extensions, argc, argv, extensionProviders, logger);
    while (opts.waitForDebugger && !stopInDebugger()) {
        // spin
    }
#ifndef SORBET_REALMAIN_MIN
    StatsD::addExtraTags(opts.metricsExtraTags);
#endif
    if (opts.stdoutHUPHack) {
        startHUPMonitor();
    }
    if (!opts.debugLogFile.empty()) {
        // LSP could run for a long time. Rotate log files, and trim at 1 GiB. Keep around 3 log files.
        // Cast first number to size_t to prevent integer multiplication.
        // TODO(jvilk): Reduce size once LSP logging is less chunderous.
        auto fileSink =
            make_shared<spdlog::sinks::rotating_file_sink_mt>(opts.debugLogFile, ((size_t)1) * 1024 * 1024 * 1024, 3);
        fileSink->set_level(spd::level::debug);
        { // replace console & fatal loggers
            vector<spd::sink_ptr> sinks{stderrColorSink, fileSink};
            auto combinedLogger = make_shared<spd::logger>("consoleAndFile", begin(sinks), end(sinks));
            combinedLogger->flush_on(spdlog::level::err);
            combinedLogger->set_level(spd::level::trace); // pass through everything, let the sinks decide

            spd::register_logger(combinedLogger);
            fatalLogger = combinedLogger;
            logger = combinedLogger;
        }
        { // replace type error logger
            vector<spd::sink_ptr> sinks{stderrColorSink, fileSink};
            auto combinedLogger = make_shared<spd::logger>("typeDiagnosticsAndFile", begin(sinks), end(sinks));
            spd::register_logger(combinedLogger);
            combinedLogger->set_level(spd::level::trace); // pass through everything, let the sinks decide
            typeErrorsConsole = combinedLogger;
        }
    }
    // Use a custom formatter so we don't get a default newline

    switch (opts.logLevel) {
        case 0:
            stderrColorSink->set_level(spd::level::info);
            break;
        case 1:
            stderrColorSink->set_level(spd::level::debug);
            logger->set_pattern("[T%t][%Y-%m-%dT%T.%f] %v");
            logger->debug("Debug logging enabled");
            break;
        default:
            stderrColorSink->set_level(spd::level::trace);
            logger->set_pattern("[T%t][%Y-%m-%dT%T.%f] %v");
            logger->trace("Trace logging enabled");
            break;
    }

    {
        string argsConcat(argv[0]);
        for (int i = 1; i < argc; i++) {
            absl::StrAppend(&argsConcat, " ", argv[i]);
        }
        logger->debug("Running sorbet version {} with arguments: {}", sorbet_full_version_string, argsConcat);
        if (!sorbet_is_release_build && !opts.silenceDevMessage &&
            std::getenv("SORBET_SILENCE_DEV_MESSAGE") == nullptr) {
            logger->info("👋 Hey there! Heads up that this is not a release build of sorbet.\n"
                         "Release builds are faster and more well-supported by the Sorbet team.\n"
                         "Check out the README to learn how to build Sorbet in release mode.\n"
                         "To forcibly silence this error, either pass --silence-dev-message,\n"
                         "or set SORBET_SILENCE_DEV_MESSAGE=1 in your shell environment.\n");
        }
    }
    unique_ptr<WorkerPool> workers = WorkerPool::create(opts.threads, *logger);

    auto errorFlusher = make_shared<core::ErrorFlusherStdout>();
    unique_ptr<core::GlobalState> gs =
        make_unique<core::GlobalState>((make_shared<core::ErrorQueue>(*typeErrorsConsole, *logger, errorFlusher)));
    gs->pathPrefix = opts.pathPrefix;
    gs->errorUrlBase = opts.errorUrlBase;
    gs->semanticExtensions = move(extensions);
    vector<ast::ParsedFile> indexed;

    gs->requiresAncestorEnabled = opts.requiresAncestorEnabled;

    logger->trace("building initial global state");
    unique_ptr<const OwnedKeyValueStore> kvstore = cache::maybeCreateKeyValueStore(opts);
    payload::createInitialGlobalState(gs, opts, kvstore);
    if (opts.silenceErrors) {
        gs->silenceErrors = true;
    }
    if (opts.autocorrect) {
        gs->autocorrect = true;
    }
    if (opts.print.isAutogen()) {
        gs->runningUnderAutogen = true;
    }
    if (opts.censorForSnapshotTests) {
        gs->censorForSnapshotTests = true;
    }
    if (opts.sleepInSlowPath) {
        gs->sleepInSlowPath = true;
    }
    gs->preallocateTables(opts.reserveClassTableCapacity, opts.reserveMethodTableCapacity,
                          opts.reserveFieldTableCapacity, opts.reserveTypeArgumentTableCapacity,
                          opts.reserveTypeMemberTableCapacity, opts.reserveUtf8NameTableCapacity,
                          opts.reserveConstantNameTableCapacity, opts.reserveUniqueNameTableCapacity);
    for (auto code : opts.isolateErrorCode) {
        gs->onlyShowErrorClass(code);
    }
    for (auto code : opts.suppressErrorCode) {
        gs->suppressErrorClass(code);
    }
    if (opts.noErrorSections) {
        gs->includeErrorSections = false;
    }
    gs->ruby3KeywordArgs = opts.ruby3KeywordArgs;
    if (!opts.stripeMode) {
        // Definitions in multiple locations interact poorly with autoloader this error is enforced in Stripe code.
        if (opts.isolateErrorCode.empty()) {
            gs->suppressErrorClass(core::errors::Namer::MultipleBehaviorDefs.code);
        }
    }
    if (!opts.reportAmbiguousDefinitionErrors) {
        // TODO (aadi-stripe, 1/16/2022): Determine whether this error should always be reported.
        if (opts.isolateErrorCode.empty()) {
            gs->suppressErrorClass(core::errors::Resolver::AmbiguousDefinitionError.code);
        }
    }
    if (opts.suggestTyped) {
        gs->ignoreErrorClassForSuggestTyped(core::errors::Infer::SuggestTyped.code);
        gs->ignoreErrorClassForSuggestTyped(core::errors::Resolver::SigInFileWithoutSigil.code);
        if (!opts.stripeMode) {
            gs->ignoreErrorClassForSuggestTyped(core::errors::Namer::MultipleBehaviorDefs.code);
        }
    }
    gs->suggestUnsafe = opts.suggestUnsafe;

    logger->trace("done building initial global state");

    unique_ptr<core::GlobalState> gsForMinimize;
    if (!opts.minimizeRBI.empty()) {
        // Copy GlobalState after createInitialGlobalState and option handling, but before rest of
        // pipeline, so that it represents an "empty" GlobalState.
        gsForMinimize = gs->deepCopy();
    }

    if (opts.runLSP) {
#ifdef SORBET_REALMAIN_MIN
        logger->warn("LSP is disabled in sorbet-orig for faster builds");
        return 1;
#else
        logger->debug("Starting sorbet version {} in LSP server mode. "
                      "Talk ‘\\r\\n’-separated JSON-RPC to me. "
                      "More details at https://microsoft.github.io/language-server-protocol/specification."
                      "If you're developing an LSP extension to some editor, make sure to run sorbet with `-v` flag,"
                      "it will enable outputing the LSP session to stderr(`Write: ` and `Read: ` log lines)",
                      sorbet_full_version_string);

        auto output = make_shared<lsp::LSPStdout>(logger);
        lsp::LSPLoop loop(move(gs), *workers, make_shared<lsp::LSPConfiguration>(opts, output, logger),
                          OwnedKeyValueStore::abort(move(kvstore)));
        gs = loop.runLSP(make_shared<lsp::LSPFDInput>(logger, STDIN_FILENO)).value_or(nullptr);
#endif
    } else {
        Timer timeall(logger, "wall_time");
        vector<core::FileRef> inputFiles;
        logger->trace("Files: ");

        if (!opts.storeState.empty()) {
            // Compute file hashes for payload files (which aren't part of inputFiles) for LSP
            hashing::Hashing::computeFileHashes(gs->getFiles(), *logger, *workers, opts);
        }

        { inputFiles = pipeline::reserveFiles(gs, opts.inputFileNames); }

        {
            core::UnfreezeFileTable fileTableAccess(*gs);
            if (!opts.inlineInput.empty()) {
                prodCounterAdd("types.input.bytes", opts.inlineInput.size());
                prodCounterInc("types.input.lines");
                prodCounterInc("types.input.files");
                auto input = opts.inlineInput;
                if (core::File::fileStrictSigil(opts.inlineInput) == core::StrictLevel::None) {
                    // put it at the end so as to not upset line numbers
                    input += "\n# typed: true";
                }
                auto file = gs->enterFile(string("-e"), input);
                inputFiles.emplace_back(file);
            }
        }

        {
            if (!opts.storeState.empty() || opts.forceHashing) {
                // Calculate file hashes alongside indexing when --store-state is specified for LSP mode
                indexed = hashing::Hashing::indexAndComputeFileHashes(gs, opts, *logger, inputFiles, *workers, kvstore);
            } else {
                indexed = pipeline::index(*gs, inputFiles, opts, *workers, kvstore);
            }
            if (gs->hadCriticalError()) {
                gs->errorQueue->flushAllErrors(*gs);
            }
        }
        cache::maybeCacheGlobalStateAndFiles(OwnedKeyValueStore::abort(move(kvstore)), opts, *gs, *workers, indexed);

        if (gs->runningUnderAutogen) {
#ifdef SORBET_REALMAIN_MIN
            logger->warn("Autogen is disabled in sorbet-orig for faster builds");
            return 1;
#else
            gs->suppressErrorClass(core::errors::Namer::MethodNotFound.code);
            gs->suppressErrorClass(core::errors::Namer::RedefinitionOfMethod.code);
            gs->suppressErrorClass(core::errors::Namer::InvalidClassOwner.code);
            gs->suppressErrorClass(core::errors::Namer::ModuleKindRedefinition.code);
            gs->suppressErrorClass(core::errors::Resolver::StubConstant.code);
            gs->suppressErrorClass(core::errors::Resolver::RecursiveTypeAlias.code);

            indexed = pipeline::package(*gs, move(indexed), opts, *workers);
            indexed = move(pipeline::name(*gs, move(indexed), opts, *workers).result());

            autogen::AutoloaderConfig autoloaderCfg;
            {
                core::UnfreezeNameTable nameTableAccess(*gs);
                core::UnfreezeSymbolTable symbolAccess(*gs);

                indexed = resolver::Resolver::runConstantResolution(*gs, move(indexed), *workers);
                autoloaderCfg = autogen::AutoloaderConfig::enterConfig(*gs, opts.autoloaderConfig);
            }

            runAutogen(*gs, opts, autoloaderCfg, *workers, indexed);
#endif
        } else {
            indexed = move(pipeline::resolve(gs, move(indexed), opts, *workers).result());
            if (gs->hadCriticalError()) {
                gs->errorQueue->flushAllErrors(*gs);
            }
            pipeline::typecheck(gs, move(indexed), opts, *workers, /* cancelable */ false, nullopt,
                                /* presorted */ false, /* intentionallyLeakASTs */ !sorbet::emscripten_build);
            if (gs->hadCriticalError()) {
                gs->errorQueue->flushAllErrors(*gs);
            }
        }

        if (!opts.minimizeRBI.empty()) {
#ifdef SORBET_REALMAIN_MIN
            logger->warn("--minimize-rbi is disabled in sorbet-orig for faster builds");
            return 1;
#else
            // In the future, we might consider making minimizeRBI be a repeatable option, and run
            // this block once for each input file.
            // The trick there is that they would all currently output to the same file, even for
            // multiple input files if we assume the naive implementation, which might not be the
            // API we want to expose.
            Minimize::indexAndResolveForMinimize(gs, gsForMinimize, opts, *workers, opts.minimizeRBI);
            Minimize::writeDiff(*gs, *gsForMinimize, opts.print.MinimizeRBI);
#endif
        }

        if (opts.suggestTyped) {
            for (auto &filename : opts.inputFileNames) {
                core::FileRef file = gs->findFileByPath(filename);
                if (!file.exists()) {
                    continue;
                }

                if (file.data(*gs).minErrorLevel() <= core::StrictLevel::Ignore) {
                    continue;
                }
                if (file.data(*gs).originalSigil > core::StrictLevel::Max) {
                    // don't change the sigil on "special" files
                    continue;
                }
                auto minErrorLevel = levelMinusOne(file.data(*gs).minErrorLevel());
                if (file.data(*gs).originalSigil == minErrorLevel) {
                    continue;
                }
                minErrorLevel = levelToRecommendation(minErrorLevel);
                if (file.data(*gs).originalSigil == minErrorLevel) {
                    // if the file could be strong, but is only marked strict, ensure that we don't reccomend that it be
                    // marked strict.
                    continue;
                }
                auto loc = findTyped(gs, file);
                if (auto e = gs->beginError(loc, core::errors::Infer::SuggestTyped)) {
                    auto sigil = levelToSigil(minErrorLevel);
                    e.setHeader("You could add `# typed: {}`", sigil);
                    e.replaceWith(fmt::format("Add `typed: {}` sigil", sigil), loc, "# typed: {}\n", sigil);
                }
            }
        }

        gs->errorQueue->flushAllErrors(*gs);

        if (!opts.noErrorCount) {
            errorFlusher->flushErrorCount(gs->errorQueue->logger, gs->errorQueue->nonSilencedErrorCount);
        }
        if (opts.autocorrect) {
            errorFlusher->flushAutocorrects(*gs, *opts.fs);
        }
        logger->trace("sorbet done");

        if (!opts.storeState.empty()) {
            gs->markAsPayload();
            FileOps::write(opts.storeState.c_str(), core::serialize::Serializer::store(*gs));
        }

        auto untypedSources = getAndClearHistogram("untyped.sources");
        if (opts.suggestSig) {
            ENFORCE(sorbet::debug_mode);
            vector<pair<string, int>> withNames;
            long sum = 0;
            for (auto e : untypedSources) {
                withNames.emplace_back(core::SymbolRef::fromRaw(e.first).showFullName(*gs), e.second);
                sum += e.second;
            }
            fast_sort(withNames, [](const auto &lhs, const auto &rhs) -> bool { return lhs.second > rhs.second; });
            for (auto &p : withNames) {
                logger->error("Typing `{}` would impact {}% callsites({} out of {}).", p.first, p.second * 100.0 / sum,
                              p.second, sum);
            }
        }
    }

#ifdef SORBET_REALMAIN_MIN
    if (opts.enableCounters || !opts.statsdHost.empty() || !opts.webTraceFile.empty() || !opts.metricsFile.empty()) {
        logger->warn("Metrics are disabled in sorbet-orig for faster builds");
        return 1;
    }
#else
    StatsD::addStandardMetrics();

    if (opts.enableCounters) {
        logger->warn("" + getCounterStatistics());
    } else {
        logger->debug("" + getCounterStatistics());
    }

    auto counters = getAndClearThreadCounters();

    if (!opts.statsdHost.empty()) {
        auto prefix = opts.statsdPrefix;
        if (opts.runLSP) {
            prefix += ".lsp";
        }
        StatsD::submitCounters(counters, opts.statsdHost, opts.statsdPort, prefix + ".counters");
    }
    if (!opts.webTraceFile.empty()) {
        web_tracer_framework::Tracing::storeTraces(counters, opts.webTraceFile);
    }

    if (!opts.metricsFile.empty()) {
        auto metrics = core::Proto::toProto(counters, opts.metricsPrefix);
        string status;
        if (gs->hadCriticalError()) {
            status = "Error";
        } else if (returnCode != 0) {
            status = "Failure";
        } else {
            status = "Success";
        }

        metrics.set_repo(opts.metricsRepo);
        metrics.set_branch(opts.metricsBranch);
        metrics.set_sha(opts.metricsSha);
        metrics.set_status(status);

        auto json = core::Proto::toJSON(metrics);

        // Create output directory if it doesn't exist
        try {
            opts.fs->writeFile(opts.metricsFile, json);
        } catch (FileNotFoundException e) {
            logger->error("Cannot write metrics file at `{}`", opts.metricsFile);
        }
    }
#endif
    if (!gs || gs->hadCriticalError() || (gsForMinimize && gsForMinimize->hadCriticalError())) {
        returnCode = 10;
    } else if (returnCode == 0 && gs->totalErrors() > 0 && !opts.supressNonCriticalErrors) {
        returnCode = 1;
    }

    opts.flushPrinters();

    if (!sorbet::emscripten_build) {
        // Let it go: leak memory so that we don't need to call destructors
        // (Although typecheck leaks these, autogen goes thru a different codepath.)
        for (auto &e : indexed) {
            intentionallyLeakMemory(e.tree.release());
        }
        intentionallyLeakMemory(gs.release());
        intentionallyLeakMemory(gsForMinimize.release());
    }

    // je_malloc_stats_print(nullptr, nullptr, nullptr); // uncomment this to print jemalloc statistics

    return returnCode;
}

} // namespace sorbet::realmain
