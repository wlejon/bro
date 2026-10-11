#pragma once

#include <string>
#include <vector>

namespace bro::util {

/// The directories model weights are looked for in, in search order, each
/// absolute and listed once (docs/models-api.js):
///   1. the entries of the BRO_MODELS environment variable,
///   2. the entries of `setting` (the `models.dirs` bro setting's value),
///   3. userModelsDir(), always last.
/// The first two are path lists in the platform's PATH form (';' between
/// entries on Windows, ':' elsewhere); empty entries are skipped, and a
/// relative entry is taken against the working directory. A directory need
/// not exist to be listed: the list says where bro looks.
std::vector<std::string> modelDirs(const std::string& setting);

/// The per-user models directory, absolute: `<BRO_APP_HOME>/models` when that
/// variable is set (a scratch profile), else `<userDataDir()>/models`. Where
/// bro.models.fetch installs a model.
std::string userModelsDir();

/// The first `<dir>/<name>` that is an existing directory, absolute, or
/// empty when there is none. `name` is a relative path of one or more
/// segments ("sam-vit-base", "sam2.1/hiera-tiny"); an empty or absolute name,
/// or one with a ".." segment, finds nothing.
std::string findModel(const std::string& name, const std::vector<std::string>& dirs);

/// Whether `name` is one findModel looks for (see above).
bool isModelName(const std::string& name);

} // namespace bro::util
