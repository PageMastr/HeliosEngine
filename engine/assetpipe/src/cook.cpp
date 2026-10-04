// Cooking one asset through the local DDC (see cook.h).

#include "helios/assetpipe/cook.h"

#include "helios/core/log.h"

namespace helios::assetpipe {

Result<CookResult> cookAsset(const CookRequest& request) {
    if (!request.importers || !request.ddc) return Error{ErrorCode::InvalidArgument, "cookAsset: no registry or DDC"};
    CookResult result;
    HELIOS_TRY_ASSIGN(result.meta, loadMeta(request.root, request.path, *request.importers));
    const ImporterInfo& importer = *request.importers->find(result.meta.importer); // loadMeta validated it
    HELIOS_TRY_ASSIGN(const std::vector<u8> source, fs::readFile(request.root / fs::pathFromUtf8(request.path)));

    DdcKeyInputs inputs;
    inputs.builder = importer.id;
    inputs.builderVersion = importer.version;
    inputs.sourceHash = hash128(source.data(), source.size());
    inputs.settings = result.meta.settings;
    inputs.settingsLayout = importer.settings ? importer.settings->layoutHash : 0;
    inputs.platform = request.platform;
    result.key = makeDdcKey(inputs);

    if (auto cached = request.ddc->get(result.key)) {
        result.product = std::move(*cached);
        result.hit = true;
        return result;
    } else if (cached.error().code != ErrorCode::NotFound) {
        HELIOS_LOG_WARN("cook {}: DDC entry {} unusable ({}); rebuilding", request.path, result.key.toHex(),
                        cached.error().toString());
    }
    if (!importer.build) {
        return makeError(ErrorCode::Unsupported, "cook {}: importer '{}' has no build step", request.path, importer.id);
    }
    const BuildContext context{source, result.meta, result.meta.settings, request.platform};
    HELIOS_TRY_ASSIGN(result.product, importer.build(context));
    if (auto stored = request.ddc->put(result.key, result.product); stored) {
        result.stored = true;
    } else {
        HELIOS_LOG_WARN("cook {}: DDC put failed: {}", request.path, stored.error().toString());
    }
    return result;
}

} // namespace helios::assetpipe
