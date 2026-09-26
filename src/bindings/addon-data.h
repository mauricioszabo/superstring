#include <napi.h>

#ifndef SUPERSTRING_ADDON_DATA_H_
#define SUPERSTRING_ADDON_DATA_H_

class AddonData final {
public:
  explicit AddonData(Napi::Env _env) {}

  // MarkerIndexWrapper
  Napi::FunctionReference marker_index_wrapper_constructor;

  // PatchWrapper
  Napi::FunctionReference patch_wrapper_constructor;
  Napi::FunctionReference change_wrapper_constructor;

  // TextBufferSnapshotWrapper
  Napi::FunctionReference text_buffer_snapshot_wrapper_constructor;

  // TextBufferWrapper
  Napi::FunctionReference text_buffer_wrapper_constructor;
  Napi::FunctionReference regex_constructor;
  Napi::FunctionReference subsequence_match_constructor;

  // Tree-sitter
  Napi::FunctionReference tree_sitter_query_constructor;
  Napi::FunctionReference syntax_node_constructor;
  Napi::FunctionReference tree_sitter_tree_constructor;

  // TextReader
  Napi::FunctionReference text_reader_constructor;

  // TextWriter
  Napi::FunctionReference text_writer_constructor;
};

#endif // SUPERSTRING_ADDON_DATA_H_
