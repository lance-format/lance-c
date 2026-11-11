// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <lance/lance.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
    ArrowSchema top_schema{};
    ArrowSchema id_schema{};
    ArrowSchema* schema_children[1]{};
    ArrowArray top_array{};
    ArrowArray id_array{};
    ArrowArray* array_children[1]{};
    const void* top_buffers[1]{};
    const void* id_buffers[2]{};
    std::array<int32_t, 3> ids{{1, 2, 3}};
    int emitted = 0;
    ArrowArrayStream stream{};

    IntStream() {
        schema_children[0] = &id_schema;
        top_schema.format = "+s";
        top_schema.n_children = 1;
        top_schema.children = schema_children;
        top_schema.release = noop_schema_release;
        id_schema.format = "i";
        id_schema.name = "id";
        id_schema.release = noop_schema_release;

        id_buffers[0] = nullptr;
        id_buffers[1] = ids.data();
        id_array.length = static_cast<int64_t>(ids.size());
        id_array.n_buffers = 2;
        id_array.buffers = id_buffers;
        id_array.release = noop_array_release;

        array_children[0] = &id_array;
        top_array.length = static_cast<int64_t>(ids.size());
        top_array.n_buffers = 1;
        top_buffers[0] = nullptr;
        top_array.buffers = top_buffers;
        top_array.n_children = 1;
        top_array.children = array_children;
        top_array.release = noop_array_release;
        stream.get_schema = [](ArrowArrayStream* raw, ArrowSchema* out) {
            auto* self = static_cast<IntStream*>(raw->private_data);
            *out = self->top_schema;
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
            *out = self->top_array;
            out->release = noop_array_release;
            return 0;
        };
        stream.get_last_error = [](ArrowArrayStream*) -> const char* { return nullptr; };
        stream.release = [](ArrowArrayStream* raw) { raw->release = nullptr; };
        stream.private_data = this;
    }

    void reset() {
        emitted = 0;
        top_array.release = noop_array_release;
        stream.get_schema = [](ArrowArrayStream* raw, ArrowSchema* out) {
            auto* self = static_cast<IntStream*>(raw->private_data);
            *out = self->top_schema;
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
            *out = self->top_array;
            out->release = noop_array_release;
            return 0;
        };
        stream.get_last_error = [](ArrowArrayStream*) -> const char* { return nullptr; };
        stream.release = [](ArrowArrayStream* raw) { raw->release = nullptr; };
        stream.private_data = this;
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

int main(int argc, char** argv) {
    try {
        require(argc > 1, "missing test root argument");
        const std::string root =
            (std::filesystem::path(argv[1]) / "backend").string();
        std::filesystem::create_directories(root);

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
        {
            auto first = lance::Namespace::custom(custom_spec);
            auto second = lance::Namespace::custom(custom_spec);
            const auto first_id = first.id();
            require(!first_id.empty(), "default custom identity is nonempty");
            require(first.id() == first_id, "default custom identity is stable");
            require(first_id != second.id(), "default custom clients have distinct identities");
        }
        custom_spec.namespace_id = "custom-cpp";
        lance::Namespace custom = lance::Namespace::custom(custom_spec);
        require(custom.id() == "custom-cpp", "custom id");
        require(custom.call("describe_table", "{}").has_json(), "custom callback");
        bool unsupported = false;
        try {
            custom.call("list_tables", "{}");
        } catch (const lance::Error& error) {
            unsupported = error.code == LANCE_ERR_NOT_SUPPORTED;
        }
        require(unsupported, "custom Unsupported");

        std::map<std::string, int> dispatch_operations;
        lance::Namespace::CustomDispatchSpec dispatch_spec;
        dispatch_spec.dispatch =
            [&dispatch_operations](const std::string& operation, const std::string&) {
                ++dispatch_operations[operation];
                if (operation == "describe_table_version") {
                    return std::string(
                        R"({"version":{"version":1,"manifest_path":"manifest"}})");
                }
                if (operation == "list_table_versions") {
                    return std::string(R"({"versions":[]})");
                }
                return std::string("{}");
            };
        dispatch_spec.namespace_id = "custom-dispatch-cpp";
        lance::Namespace dispatch_custom = lance::Namespace::custom_dispatch(dispatch_spec);
        require(dispatch_custom.id() == "custom-dispatch-cpp", "dispatch custom id");
        require(dispatch_custom.call("describe_table", "{}").has_json(),
                "dispatch describe_table");
        require(dispatch_custom.call("describe_table_version", "{}").has_json(),
                "dispatch describe_table_version");
        require(dispatch_custom.call(
                    "create_table_version",
                    R"({"id":["workspace","table"],"version":1,"manifest_path":"manifest"})")
                    .has_json(),
                "dispatch create_table_version");
        require(dispatch_custom.call("list_table_versions", "{}").has_json(),
                "dispatch list_table_versions");
        require(dispatch_operations["describe_table"] == 1, "dispatch operation describe_table");
        require(dispatch_operations["describe_table_version"] == 1,
                "dispatch operation describe_table_version");
        require(dispatch_operations["create_table_version"] == 1,
                "dispatch operation create_table_version");
        require(dispatch_operations["list_table_versions"] == 1,
                "dispatch operation list_table_versions");

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
