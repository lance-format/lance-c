// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <lance/lance.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void noop_schema_release(ArrowSchema* schema) { schema->release = nullptr; }
void noop_array_release(ArrowArray* array) { array->release = nullptr; }

struct IntStream {
    ArrowSchema schema{};
    ArrowArray array{};
    const void* buffers[2]{};
    std::array<int32_t, 3> ids{{1, 2, 3}};
    int emitted = 0;
    ArrowArrayStream stream{};

    IntStream() {
        schema.format = "i";
        schema.name = "id";
        array.length = static_cast<int64_t>(ids.size());
        array.n_buffers = 2;
        buffers[0] = nullptr;
        buffers[1] = ids.data();
        array.buffers = buffers;
        stream.get_schema = [](ArrowArrayStream* raw, ArrowSchema* out) {
            auto* self = static_cast<IntStream*>(raw->private_data);
            *out = self->schema;
            out->release = noop_schema_release;
            return 0;
        };
        stream.get_next = [](ArrowArrayStream* raw, ArrowArray* out) {
            auto* self = static_cast<IntStream*>(raw->private_data);
            if (self->emitted) {
                *out = {};
                return 0;
            }
            self->emitted = 1;
            *out = self->array;
            out->release = noop_array_release;
            return 0;
        };
        stream.get_last_error = [](ArrowArrayStream*) -> const char* { return nullptr; };
        stream.release = [](ArrowArrayStream* raw) { raw->release = nullptr; };
        stream.private_data = this;
    }

    void reset() {
        emitted = 0;
        array.release = noop_array_release;
        stream.release = [](ArrowArrayStream* raw) { raw->release = nullptr; };
    }
};

bool contains(const std::string& value, const std::string& needle) {
    return value.find(needle) != std::string::npos;
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_table(
    const lance::Namespace& ns,
    const std::vector<std::string>& table_id,
    lance::WriteMode mode,
    IntStream& fixture) {
    fixture.reset();
    auto dataset = lance::Dataset::write_namespace(
        ns, table_id, &fixture.stream, mode, lance::WriteOptions{});
    require(dataset.count_rows() > 0, "written dataset has rows");
}

}  // namespace

int main() {
    try {
        char root_template[] = "/tmp/lance-ns-cpp-XXXXXX";
        if (mkdtemp(root_template) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        const std::string root = std::string(root_template) + "/backend";

        lance::Namespace backend = lance::Namespace::directory({
            {"root", root},
            {"manifest_enabled", "true"},
            {"table_version_tracking_enabled", "true"},
            {"ops_metrics_enabled", "true"},
        });
        require(!backend.id().empty(), "namespace id");
        backend.call("create_namespace", R"({"id":["workspace"]})");

        IntStream fixture;
        const std::vector<std::string> table_id{"workspace", "numbers"};
        write_table(backend, table_id, lance::WriteMode::Create, fixture);
        write_table(backend, table_id, lance::WriteMode::Append, fixture);
        write_table(backend, table_id, lance::WriteMode::Overwrite, fixture);

        auto count = backend.call(
            "count_table_rows", R"({"id":["workspace","numbers"]})");
        require(count.has_json() && *count.json() == "3", "overwrite count");

        auto opened = lance::Dataset::open_namespace(backend, table_id);
        require(opened.count_rows() == 3, "reopen count");

        auto metrics = backend.ops_metrics_json();
        require(contains(metrics, ":"), "directory metrics");
        backend.reset_ops_metrics();
        metrics = backend.ops_metrics_json();
        require(metrics == "{}" || contains(metrics, "\"0\""), "metrics reset");

        // RAII + four managed-version callbacks. Unimplemented operations keep
        // native Unsupported and preserve the native code separately.
        lance::Namespace::CustomClientSpec custom_spec;
        custom_spec.describe_table = [](const std::string&) {
            return std::string("{}");
        };
        custom_spec.describe_table_version = [](const std::string&) {
            return std::string("{}");
        };
        custom_spec.create_table_version = [](const std::string&) {
            return std::string("{}");
        };
        custom_spec.list_table_versions = [](const std::string&) {
            return std::string(R"({"versions":[]})");
        };
        custom_spec.namespace_id = "custom-cpp";
        lance::Namespace custom = lance::Namespace::custom(custom_spec);
        require(custom.id() == "custom-cpp", "custom id");
        require(custom.call("describe_table", "{}").has_json(), "custom callback");
        bool unsupported = false;
        try {
            custom.call("list_tables", "{}");
        } catch (const lance::NamespaceError& error) {
            unsupported = error.code == LANCE_ERR_NOT_SUPPORTED &&
                          error.native_code == 0 && !error.native_message.empty();
        }
        require(unsupported, "custom native Unsupported");

        // Native REST adapter + REST client through the public wrappers.
        lance::RestAdapter adapter = lance::RestAdapter::start(backend, "127.0.0.1", 0);
        require(adapter.port() != 0, "adapter port");
        std::map<std::string, int> context_calls;
        lance::DynamicContextProvider context(
            [&context_calls](const std::string& operation, const std::string&) {
                ++context_calls[operation];
                return std::map<std::string, std::string>{
                    {"headers.X-Namespace-Test", "cpp"}};
            });
        lance::Namespace rest = lance::Namespace::rest(
            {{"uri", "http://127.0.0.1:" + std::to_string(adapter.port())}},
            &context);
        auto rest_count = rest.call(
            "count_table_rows", R"({"id":["workspace","numbers"]})");
        require(rest_count.has_json() && *rest_count.json() == "3", "REST count");
        require(!context_calls.empty(), "context provider called");
        auto rest_opened = lance::Dataset::open_namespace(rest, table_id);
        require(rest_opened.count_rows() == 3, "REST reopen");

        bool rest_not_found = false;
        try {
            rest.call("describe_table", R"({"id":["workspace","missing"]})");
        } catch (const lance::NamespaceError& error) {
            rest_not_found = error.code == LANCE_ERR_NOT_FOUND &&
                             error.native_code == 4;
        }
        require(rest_not_found, "REST NotFound native code");

        adapter.shutdown();
        require(adapter.port() == 0, "adapter stopped");

        std::cout << "native namespace C++ test passed" << std::endl;
        return 0;
    } catch (const lance::NamespaceError& error) {
        std::cerr << "namespace error: " << error.what()
                  << " native=" << error.native_code << " " << error.native_message
                  << std::endl;
    } catch (const lance::Error& error) {
        std::cerr << "lance error: " << error.what() << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << std::endl;
    }
    return 1;
}
