#pragma once

// Installing a model folder from the network: what bro.models.fetch and
// bro.models.available run (docs/models-api.js). A job runs on its own
// thread over plain C++ state; the JS side polls snapshot() and never blocks.
//
// SOURCES. An ordered list of Hugging Face datasets laid out as model
// folders (wlejon/brovisionml-data, wlejon/brosoundml-data, ...). An entry is
// either `<owner>/<repo>` (on https://huggingface.co) or a full URL
// `<endpoint>/datasets/<owner>/<repo>` for a mirror or a test server that
// answers the same paths. A folder `<name>` is listed through
//   <endpoint>/api/datasets/<owner>/<repo>/tree/main/<name>
// (each subdirectory listed in turn) and a file downloaded from
//   <endpoint>/datasets/<owner>/<repo>/resolve/main/<path>
// The first source whose listing has the folder supplies all of it.
//
// INSTALLING. Files stream into `<root>/.bro-fetch/<name>/`, each as
// `<file>.part` until its bytes are complete and verified, then renamed; when
// every file is in, the folder is renamed to `<root>/<name>` in one step, so
// bro.models.find never sees a half-installed model. A file is verified
// against the listing: an LFS file's sha256 (lfs.oid), any other file's git
// blob id (sha1 over "blob <size>\0" + bytes). A transfer that fails is
// retried, resuming from the bytes already on disk (an HTTP Range request);
// an interrupted fetch leaves its .part files for the next one to resume. A
// file whose hash does not match is deleted and the fetch fails.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bro::util {

struct ModelSource {
    std::string endpoint;  // "https://huggingface.co"
    std::string repo;      // "wlejon/brovisionml-data"
    std::string label;     // the entry as written
};

/// The default `models.sources`: "wlejon/brovisionml-data wlejon/brosoundml-data".
const char* defaultModelSources();

/// The entries of `setting` (separated by whitespace or commas; an empty
/// setting means defaultModelSources()). An entry that is neither form is
/// skipped.
std::vector<ModelSource> parseModelSources(const std::string& setting);

/// Where a fetch of `name` into `root` stages its files before the rename.
std::string modelFetchStagingDir(const std::string& root, const std::string& name);

class ModelFetchJob {
public:
    enum class State { Running, Done, Failed, Cancelled };

    struct Snapshot {
        State state = State::Running;
        uint64_t done = 0;   // bytes on disk of the folder's files
        uint64_t total = 0;  // the folder's size, once listed (0 before)
        int files = 0;       // the folder's file count, once listed
        std::string file;    // the file being transferred
        std::string source;  // the source that has the folder (label)
        std::string dir;     // Done + install: the installed folder
        std::string error;   // Failed: what went wrong
        std::string code;    // Failed: "not-found" | "network" | "integrity" | "io" | "unsupported"
    };

    /// Starts the job's thread. `listOnly`: find the folder in a source and
    /// stop (bro.models.available); otherwise install it under `root`.
    /// `after`: a job on the same staging folder still stopping (cancelled);
    /// this one waits for it to settle before touching the files.
    ModelFetchJob(std::string name, std::string root, std::vector<ModelSource> sources, bool listOnly,
                  std::shared_ptr<ModelFetchJob> after = nullptr);
    ~ModelFetchJob();  // cancels and joins

    ModelFetchJob(const ModelFetchJob&) = delete;
    ModelFetchJob& operator=(const ModelFetchJob&) = delete;

    const std::string& name() const { return name_; }
    bool listOnly() const { return listOnly_; }

    void cancel() { cancel_.store(true); }
    bool cancelled() const { return cancel_.load(); }

    Snapshot snapshot() const;

    // The worker's own helpers, public only for the implementation file.
    void setFile(const std::string& f);
    void addDone(uint64_t n) { done_.fetch_add(n); }

private:
    void run();
    void finish(State st, std::string dir, std::string error, std::string code);

    std::string name_, root_;
    std::vector<ModelSource> sources_;
    bool listOnly_;
    std::shared_ptr<ModelFetchJob> after_;

    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> done_{0};
    std::atomic<uint64_t> total_{0};
    std::atomic<int> files_{0};

    mutable std::mutex mu_;  // the strings and the state below
    State state_ = State::Running;
    std::string file_, source_, dir_, error_, code_;

    std::thread th_;
};

} // namespace bro::util
