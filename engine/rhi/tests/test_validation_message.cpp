// ValidationMessage::isLayerError on the CPU (every toolchain): which messages count as a validation
// layer's report. helios-rendertest's validation self-test relies on it; the first win-gpu run showed
// why it must not depend on the text (layer 1.4.363 dropped 1.3.275's "Validation Error: [" prefix).

#include <doctest/doctest.h>

#include "helios/rhi/rhi.h"

using namespace helios;
using namespace helios::rhi;

namespace {

ValidationMessage layerError() {
    ValidationMessage m;
    m.severity = ValidationMessage::Severity::Error;
    m.source = ValidationMessage::Source::Api;
    m.validation = true;
    m.id = "VUID-VkImageMemoryBarrier2-oldLayout-01197";
    m.idNumber = 0x4dae5635;
    m.text = "vkCmdPipelineBarrier2(): pDependencyInfo->pImageMemoryBarriers[0] ...";  // 1.4.363's format
    return m;
}

TEST_CASE("rhi validation message: a layer error is told apart by its fields, not its text") {
    CHECK(layerError().isLayerError());

    SUBCASE("either message ID suffices") {
        ValidationMessage byNumber = layerError();
        byNumber.id.clear();
        CHECK(byNumber.isLayerError());
        ValidationMessage byName = layerError();
        byName.idNumber = 0;
        CHECK(byName.isLayerError());
        ValidationMessage neither = layerError();
        neither.id.clear();
        neither.idNumber = 0;
        CHECK_FALSE(neither.isLayerError());
    }
    SUBCASE("the RHI's own reports are not the layer's") {
        ValidationMessage rhi = layerError();
        rhi.source = ValidationMessage::Source::Rhi;
        CHECK_FALSE(rhi.isLayerError());
    }
    SUBCASE("general and performance messages are not validation reports") {
        ValidationMessage general = layerError();
        general.validation = false;
        CHECK_FALSE(general.isLayerError());
    }
    SUBCASE("warnings and infos are not errors") {
        ValidationMessage warning = layerError();
        warning.severity = ValidationMessage::Severity::Warning;
        CHECK_FALSE(warning.isLayerError());
        ValidationMessage info = layerError();
        info.severity = ValidationMessage::Severity::Info;
        CHECK_FALSE(info.isLayerError());
    }
    SUBCASE("the loader's own messages are not a layer's, even of the validation type") {
        ValidationMessage loader = layerError();
        loader.id = "Loader Message";
        loader.idNumber = 0;
        loader.text = "vkCreateDevice: Invalid physicalDevice";
        CHECK_FALSE(loader.isLayerError());
    }
    SUBCASE("the text plays no part, old format or none") {
        ValidationMessage old = layerError();
        old.text = "Validation Error: [ VUID-VkImageMemoryBarrier2-oldLayout-01197 ] ...";  // 1.3.275
        CHECK(old.isLayerError());
        ValidationMessage empty = layerError();
        empty.text.clear();
        CHECK(empty.isLayerError());
        ValidationMessage lookalike;
        lookalike.severity = ValidationMessage::Severity::Error;
        lookalike.text = "Validation Error: [ VUID-x ] from the RHI";
        CHECK_FALSE(lookalike.isLayerError());
    }
    CHECK_FALSE(ValidationMessage{}.isLayerError());
}

} // namespace
