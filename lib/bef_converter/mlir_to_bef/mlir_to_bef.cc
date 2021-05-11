// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file implements the main entrypoints for the MLIRToBEF library.
// The converter is implemented in three phases.  The first phase identifies all
// of the strings and attributes that need to be emitted to the string/attribute
// pool.  The second phase optimizes and emits the strings and attributes to
// the file and remembers their offsets.  The third phase emits all of the
// regions in the MLIR program.
//
// MLIR ops are converted to kernel info and stored in BEF. So the term "op" is
// used in MLIR related code, and "kernel" used in BEF related code.
//===----------------------------------------------------------------------===//

#include "tfrt/bef_converter/mlir_to_bef.h"

#include <cstring>

#include "bef_attr_emitter.h"
#include "bef_compilation_units.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/OperationSupport.h"
#include "tfrt/bef_converter/bef_emitter.h"
#include "tfrt/compiler/stream_analysis.h"
#include "tfrt/core_runtime/opdefs/attributes.h"
#include "tfrt/core_runtime/opdefs/traits.h"
#include "tfrt/core_runtime/opdefs/types.h"
#include "tfrt/host_context/debug_info.h"
#include "tfrt/support/aligned_buffer.h"
#include "tfrt/support/bef_encoding.h"
#include "tfrt/support/error_util.h"
#include "tfrt/support/forward_decls.h"

#ifdef DEBUG_MLIR_TO_BEF
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

namespace tfrt {

namespace {
// This is a simple enum used to indicate success or failure in a more
// structured way than a simple bool.
enum class LogicalResult { Success, Failure };

}  // namespace

// The "tfrt.return" kernel gets special case handling in BEF files.
static bool IsReturn(mlir::Operation* op) {
  // TODO(tfrt-dev): Use C++ op type here instead of relying on string
  // comparing.
  return op->getName().getStringRef() == "tfrt.return";
}

static bool IsNativeFunc(mlir::FuncOp op) {
  return !!op->getAttr("tfrt.native");
}

static bool IsSyncFunc(mlir::FuncOp op) { return !!op->getAttr("tfrt.sync"); }

static mlir::FunctionType GetRegionFunctionType(mlir::Region* region) {
  // Emit information about the type of the function.
  auto& block = region->front();

  // Arguments.
  SmallVector<mlir::Type, 4> inputs;
  for (auto arg : block.getArguments()) inputs.push_back(arg.getType());

  // Results.
  // MLIR Regions don't have an easy way to identify results in regions, so
  // we just hard code the "tfrt.return" instruction.
  auto& last_op = block.back();
  assert(IsReturn(&last_op));

  SmallVector<mlir::Type, 4> results;
  for (auto op : last_op.getOperands()) results.push_back(op.getType());

  return mlir::FunctionType::get(region->getContext(), inputs, results);
}

//===----------------------------------------------------------------------===//
// EntityTable
//===----------------------------------------------------------------------===//

namespace {

// This table keeps track of the interesting entities (attributes, types, other
// strings) that we care about.  This is built in the first pass.
struct EntityTable {
  // Uniquing set of attributes we need to emit, kept in order so we always
  // produce a determinstic output file.
  llvm::SetVector<mlir::Attribute> attributes;

  // Uniquing set of the kernels that we need to emit.
  std::vector<string_view> kernels;
  llvm::StringMap<unsigned> kernel_ids;

  struct FunctionEntry {
    FunctionEntry(string_view name, mlir::FunctionType type, FunctionKind kind,
                  mlir::Region* region = nullptr)
        : name(name), type(type), kind(kind), region(region) {}

    string_view name;
    mlir::FunctionType type;
    FunctionKind kind;

    // If region is nullptr, then it is an external function (eg. a native
    // function).
    mlir::Region* region = nullptr;

    bool IsNative() const { return kind == FunctionKind::kNativeFunction; }

    bool IsSync() const { return kind == FunctionKind::kSyncBEFFunction; }
  };

  // List of functions that we need to emit, along with a name if they came
  // from a top level function.
  std::vector<FunctionEntry> functions;
  llvm::DenseMap<mlir::Region*, unsigned> region_function_ids;
  llvm::StringMap<unsigned> named_function_ids;

  // Types we've seen so far.
  std::vector<mlir::Type> types;
  llvm::DenseMap<mlir::Type, unsigned> type_ids;

  // All of the strings we need to emit the BEF file, an unordered
  // collection that we sort before emitting.  We use StringMap here instead of
  // StringSet because StringSet rejects empty strings for no apparent reason.
  llvm::StringMap<uint8_t> strings;

  // This is all of the filenames referred to by locations in the file.
  llvm::SmallVector<string_view, 4> location_filenames;
  llvm::StringMap<unsigned> location_filenames_index;

  // These are the locations for all operations within the file, the first
  // element of the tuple is a index into location_filenames, the second and
  // third are line/col information.
  typedef std::tuple<unsigned, unsigned, unsigned> LocationTuple;
  llvm::DenseMap<mlir::Operation*, LocationTuple> location_positions;

  llvm::DenseMap<mlir::Operation*, DebugInfoEntry> debug_info;

 public:
  LogicalResult Collect(mlir::ModuleOp module,
                        bool collect_attribute_types_and_names);
  ssize_t GetFunctionNamed(string_view name) const;

  void AddString(string_view string);
  void AddType(mlir::Type type);
  unsigned GetTypeIndex(mlir::Type type) const;

  void AddNativeFunction(mlir::FuncOp op);
  LogicalResult AddFunction(mlir::Region* region, string_view name,
                            FunctionKind func_kind);
  unsigned GetFunctionID(const mlir::Region& region) const;

  void AddKernel(mlir::Operation* kernel);
  unsigned GetKernelID(mlir::Operation* kernel) const;

  void AddLocation(mlir::Operation* op);

  void AddDebugInfo(mlir::Operation* op);

  void AddAttributeType(mlir::Attribute attr);
};

}  // namespace

void EntityTable::AddString(string_view string) { strings[string] = 0; }

// Add a type to our table, checking it by pointer to reduce string
// conversions.
void EntityTable::AddType(mlir::Type type) {
  // Ignore the type if we've seen it before.
  if (!type_ids.insert({type, types.size()}).second) return;
  types.push_back(type);

  // If it is new, remember the type name as a string.
  llvm::SmallVector<char, 64> result_str;
  llvm::raw_svector_ostream os(result_str);
  type.print(os);
  AddString(os.str());
}

unsigned EntityTable::GetTypeIndex(mlir::Type type) const {
  auto it = type_ids.find(type);
  assert(it != type_ids.end() && "unregistered type");
  return it->second;
}

void EntityTable::AddNativeFunction(mlir::FuncOp op) {
  auto function_type = op.getType();

  for (auto type : function_type.getInputs()) AddType(type);
  for (auto type : function_type.getResults()) AddType(type);

  auto name = op.getName();

  AddString(name);
  named_function_ids[name] = functions.size();
  functions.push_back(
      FunctionEntry(name, function_type, FunctionKind::kNativeFunction));
}

LogicalResult EntityTable::AddFunction(mlir::Region* region, string_view name,
                                       FunctionKind func_kind) {
  // Check to see if we support this region kind.
  if (!llvm::hasSingleElement(*region)) {
    mlir::emitError(region->getLoc())
        << "multi-block regions cannot be emitted to BEF files";
    return LogicalResult::Failure;
  }

  for (auto type : region->getArgumentTypes()) AddType(type);

  // Remember this function.
  AddString(name);
  region_function_ids[region] = functions.size();
  named_function_ids[name] = functions.size();
  functions.push_back(
      FunctionEntry(name, GetRegionFunctionType(region), func_kind, region));
  return LogicalResult::Success;
}

unsigned EntityTable::GetFunctionID(const mlir::Region& region) const {
  auto it = region_function_ids.find(&region);
  assert(it != region_function_ids.end() && "region not added to entity table");
  return it->second;
}

// Return the index of the specified function name, returning -1 if the
// function name cannot be found.
ssize_t EntityTable::GetFunctionNamed(string_view name) const {
  auto iter = named_function_ids.find(name);
  if (iter == named_function_ids.end()) return -1;
  return iter->second;
}

void EntityTable::AddKernel(mlir::Operation* kernel) {
  // Remember the kernel.
  if (!kernel_ids.insert({kernel->getName().getStringRef(), kernels.size()})
           .second)
    return;
  kernels.push_back(kernel->getName().getStringRef());

  // If we haven't seen it already, add it to the string table.
  AddString(kernel->getName().getStringRef());
}

unsigned EntityTable::GetKernelID(mlir::Operation* kernel) const {
  auto it = kernel_ids.find(kernel->getName().getStringRef());
  assert(it != kernel_ids.end() && "Unknown kernel");
  return it->second;
}

void EntityTable::AddDebugInfo(mlir::Operation* op) {
  auto debug_info_location = op->getLoc();

  // If the location is a FusedLoc, look for a NameLoc among its children.
  // TODO(b/180438663): Handle cases where there are multiple NameLoc.
  if (auto fused_loc = debug_info_location.dyn_cast<mlir::FusedLoc>()) {
    for (auto& location : fused_loc.getLocations()) {
      if (auto named_loc = location.dyn_cast<mlir::NameLoc>()) {
        debug_info_location = location;
        break;
      }
    }
  }

  // If the location is a CallSiteLoc, look whether the callee is a NameLoc.
  if (auto call_site = debug_info_location.dyn_cast<mlir::CallSiteLoc>()) {
    const auto& location = call_site.getCallee();
    if (auto named_loc = location.dyn_cast<mlir::NameLoc>()) {
      debug_info_location = location;
    }
  }

  if (auto named_loc = debug_info_location.dyn_cast<mlir::NameLoc>()) {
    DebugInfoEntry debug_info_entry = named_loc.getName().c_str();
    auto r = debug_info.try_emplace(op, debug_info_entry);
    assert(r.second);
    (void)r;
  }
}

void EntityTable::AddLocation(mlir::Operation* op) {
  auto file_line_col_location = op->getLoc();
  string_view filename = "";
  unsigned line = 0, col = 0;

  // If the location is a FusedLoc, look for a FileLineColLoc among its
  // children.
  // TODO(b/180438663): Handle cases where there are multiple FileLineColLoc.
  if (auto fused_loc = file_line_col_location.dyn_cast<mlir::FusedLoc>()) {
    for (auto& location : fused_loc.getLocations()) {
      if (auto loc = location.dyn_cast<mlir::FileLineColLoc>()) {
        file_line_col_location = loc;
        break;
      }
    }
  }

  if (auto loc = file_line_col_location.dyn_cast<mlir::FileLineColLoc>()) {
    filename = loc.getFilename();
    line = loc.getLine();
    col = loc.getColumn();
  }

  auto next_filename_index = location_filenames.size();
  auto it =
      location_filenames_index.insert({filename, next_filename_index}).first;
  if (it->second == next_filename_index) location_filenames.push_back(filename);

  auto r =
      location_positions.try_emplace(op, LocationTuple{it->second, line, col});
  assert(r.second);
  (void)r;
}

void EntityTable::AddAttributeType(mlir::Attribute attr) {
  if (auto int_type = attr.getType().dyn_cast<mlir::IntegerType>()) {
    AddType(int_type);
  }

  if (auto float_attr = attr.dyn_cast<mlir::FloatAttr>()) {
    AddType(float_attr.getType());
  }

  if (auto arr_attr = attr.dyn_cast<mlir::ArrayAttr>()) {
    for (auto attr : arr_attr.getValue()) {
      AddAttributeType(attr);
    }
  }
}

LogicalResult EntityTable::Collect(mlir::ModuleOp module,
                                   bool collect_attribute_types_and_names) {
  auto result = LogicalResult::Success;

  std::vector<std::pair<mlir::SymbolRefAttr, mlir::Location>> fn_attrs;

  module.walk(
      [&](mlir::Operation* op) {
        // Ignore the module itself, and a few specific other ops.
        if (op == module.getOperation()) return;

        // Ignore operations inside compiled modules. Symbol references into the
        // compiled modules passes to kernels as a compilation unit attribute.
        if (BefCompilationUnits::IsInCompiledModule(op)) return;

        // The return op gets special handling, ensure it is at the end of its
        // enclosing block.
        if (IsReturn(op)) {
          if (&op->getBlock()->back() != op) {
            op->emitError() << "return op must be at the end of its block";
            result = LogicalResult::Failure;
            return;
          }
          // Ignore it, return gets special handling.
          return;
        }

        AddLocation(op);
        AddDebugInfo(op);

        auto* cur_region = op->getParentRegion();

        // Notice the result and argument types of the ops.
        for (auto result : op->getResults()) AddType(result.getType());

        for (auto operand : op->getOperands()) {
          // Verify that the operand is defined inside the current region.  We
          // don't support references to outer regions.
          if (operand.getParentRegion() != cur_region) {
            op->emitError()
                << "BEF executor only supports references to kernels within"
                << " the current region";
            result = LogicalResult::Failure;
            return;
          }
        }

        // We treat functions specially, putting them into the symbol table and
        // ignoring their attributes.
        if (auto fn = llvm::dyn_cast<mlir::FuncOp>(op)) {
          if (IsNativeFunc(fn)) {
            AddNativeFunction(fn);
          } else {
            if (fn.isExternal()) {
              fn.emitError() << "external functions are not allowed";
              result = LogicalResult::Failure;
              return;
            }

            // Verify that all functions end with a return to catch a common
            // error.
            auto& last_op = fn.front().back();
            if (!IsReturn(&last_op)) {
              last_op.emitError() << "all functions need to have a tfrt.return";
              result = LogicalResult::Failure;
              return;
            }

            if (IsSyncFunc(fn)) {
              llvm::SmallSetVector<mlir::Value, 4> return_operands;
              for (auto iter : llvm::enumerate(last_op.getOperands())) {
                auto index = iter.index();
                const auto& operand = iter.value();
                if (operand.isa<mlir::BlockArgument>()) {
                  last_op.emitError() << "return value " << index
                                      << " is an argument in a sync function";
                  result = LogicalResult::Failure;
                  return;
                }

                if (!return_operands.insert(operand)) {
                  last_op.emitError() << "return value " << index
                                      << " is duplicated in a sync function";
                  result = LogicalResult::Failure;
                  return;
                }
              }
            }

            auto func_kind = IsSyncFunc(fn) ? FunctionKind::kSyncBEFFunction
                                            : FunctionKind::kBEFFunction;
            if (AddFunction(&fn.getBody(), fn.getName(), func_kind) ==
                LogicalResult::Failure) {
              result = LogicalResult::Failure;
              return;
            }
          }
        } else {
          AddKernel(op);

          // Keep track of any attributes used by this op.
          for (auto attr : op->getAttrs()) {
            // Skip cost attribute which is not used in runtime execution.
            //
            // TODO(tfrt-devs): Use attribute interface instead of hardcoding
            // here.
            if (attr.first == "_tfrt_cost") continue;

            // If this is a special attribute, ignore it.
            if (BefAttrEmitter::ClassifyAttribute(attr.first.strref()) !=
                SpecialAttribute::kUnknown)
              continue;

            // Check to make sure that this is a supported attribute, if not,
            // reject it.
            if (!BefAttrEmitter::IsSupportedAttribute(attr.second) &&
                result == LogicalResult::Success) {
              op->emitError() << "BEF files cannot encode the '" << attr.first
                              << "' attribute";
              result = LogicalResult::Failure;
              return;
            }

            // Returns a symbol ref to an executable operation (function that
            // needs to be converted to BEF). If the referenced symbol is inside
            // the compiled module returns None. All compiled operations will be
            // added to the attributes section as compilation units.
            auto bef_function_ref = [&]() -> Optional<mlir::SymbolRefAttr> {
              auto sym_attr = attr.second.dyn_cast<mlir::SymbolRefAttr>();
              if (!sym_attr) return llvm::None;

              // Check if the referenced symbol is in the compiled module.
              auto* module_op = module.getOperation();
              auto* sym_op =
                  mlir::SymbolTable::lookupSymbolIn(module_op, sym_attr);
              if (sym_op && BefCompilationUnits::IsInCompiledModule(sym_op))
                return llvm::None;

              return sym_attr;
            };

            if (auto fn_attr = bef_function_ref()) {
              // Keep track of function attributes specially so we can diagnose
              // them.
              fn_attrs.push_back({*fn_attr, op->getLoc()});

            } else {
              if (collect_attribute_types_and_names) {
                // Add attribute names and types for attribute types section and
                // attribute names section. These will be ignored by executor.
                AddString(attr.first);
                AddAttributeType(attr.second);
              }

              // Skip collecting array of function attributes.
              auto array_attr = attr.second.dyn_cast<mlir::ArrayAttr>();
              if (array_attr) {
                if (!array_attr.empty() &&
                    array_attr.begin()->dyn_cast<mlir::SymbolRefAttr>()) {
                  continue;
                }
              }

              // We ignore the name of attributes, they just get passed as
              // arguments.
              attributes.insert(attr.second);
            }
          }

          // Keep add any regions used by this op as BEF functions.
          for (auto& region : op->getRegions()) {
            if (AddFunction(&region, "", FunctionKind::kBEFFunction) ==
                LogicalResult::Failure) {
              result = LogicalResult::Failure;
              return;
            }
          }
        }
      });

  // If we're successful, check to make sure that all functions that should be
  // translated to BEF can be resolved.
  if (result == LogicalResult::Success) {
    for (auto attr_and_loc : fn_attrs) {
      if (GetFunctionNamed(attr_and_loc.first.getRootReference()) == -1) {
        mlir::emitError(attr_and_loc.second)
            << "function " << attr_and_loc.first << " not defined";
        return LogicalResult::Failure;
      }
    }
  }

  return result;
}

namespace {

// each entity is assigned.
class EntityIndex {
 public:
  unsigned GetStringOffset(string_view str) const {
    auto it = strings_.find(str);
    assert(it != strings_.end() &&
           "String didn't get added to the entity collection");
    return it->second;
  }

  void AddString(string_view str, unsigned offset) {
    assert(!strings_.count(str) && "string already exists");
    strings_.insert({str, offset});
  }

  unsigned GetAttributeOffset(mlir::Attribute attribute) const {
    auto it = attribute_offsets_.find(attribute);
    assert(it != attribute_offsets_.end() &&
           "attribute didn't get added to the entity collection");
    return it->second;
  }

  void AddAttributeOffset(mlir::Attribute attribute, unsigned offset) {
    assert(!attribute_offsets_.count(attribute) &&
           "attribute already in index");
    attribute_offsets_.insert({attribute, offset});
  }

  struct FunctionIndexEntry {
    size_t name_offset;
    size_t function_offset;
    mlir::FunctionType type;
    FunctionKind kind;
  };

  void AddFunction(string_view name, unsigned offset, mlir::FunctionType type,
                   FunctionKind kind) {
    function_index_.push_back({GetStringOffset(name), offset, type, kind});
  }

  llvm::ArrayRef<FunctionIndexEntry> GetFunctionIndex() const {
    return function_index_;
  }

  typedef EntityTable::LocationTuple LocationTuple;

  void AddLocationPosition(mlir::Operation* op, size_t offset) {
    location_position_offsets_[op] = offset;
  }

  size_t GetLocationPositionOffset(mlir::Operation* op) const {
    auto loc_it = location_position_offsets_.find(op);
    assert(loc_it != location_position_offsets_.end() && "unknown location");
    return loc_it->second;
  }

  void AddDebugInfoOffset(mlir::Operation* op, size_t offset) {
    debug_info_offset_[op] = offset;
  }

  llvm::Optional<size_t> GetDebugInfoOffset(mlir::Operation* op) const {
    auto loc_it = debug_info_offset_.find(op);
    if (loc_it != debug_info_offset_.end()) {
      return loc_it->second;
    } else {
      return llvm::None;
    }
  }

 private:
  llvm::StringMap<unsigned> strings_;
  llvm::DenseMap<mlir::Attribute, unsigned> attribute_offsets_;

  // This follows the format of the FunctionIndex section, where the first
  // element is the offset of the name in the string section, the second is the
  // offset into the function table.
  std::vector<FunctionIndexEntry> function_index_;

  // This is the location of the offsets into the section.
  llvm::DenseMap<mlir::Operation*, size_t> location_position_offsets_;

  // This is the offset of associated entry in the debug info section (if any)
  // kNoDebugInfoEntryOffset represents no entry.
  llvm::DenseMap<mlir::Operation*, DebugInfoOffset> debug_info_offset_;
};

}  // namespace

namespace {

// This is the emitter that builds a BEF into an std::vector.  This class
// contains the primitive routines used by the various specific emitters.  In
// addition to collecting the bytes contained in this piece of the BEF file,
// this tracks the alignment requirement of the contents.  If this is a
// subsection of the file, then the enclosing container is required to provide
// at least this alignment.
class BEFFileEmitter : public BefEmitter {
 public:
  static constexpr uint32_t kDummyPseudoKernelCode = 0xABABABAB;
  static constexpr uint32_t kDummyPseudoKernelLocation = 0xCDCDCDCD;

  BEFFileEmitter() {}
  BEFFileEmitter(const BEFFileEmitter&) = delete;
  BEFFileEmitter& operator=(const BEFFileEmitter&) = delete;

  void EmitSection(BEFSectionID section_id,
                   llvm::ArrayRef<uint8_t> section_data,
                   unsigned alignment = 1) {
    // Section start with an identifier.
    result_.push_back(static_cast<uint8_t>(section_id));

    // LENGTH_AND_ALIGNMENT ::= (SECTION_LENGTH << 1) | (SECTION_ALIGNMENT_FLAG)
    const auto shifted_section_length = (section_data.size() << 1);
    bool length_emitted = false;
    if (alignment > 1) {
      auto offset = size() + GetSizeOfVbrInt(shifted_section_length);
      if (offset % alignment != 0) {
        // Emit section length with alignment constraint.
        EmitVbrInt(shifted_section_length | 1);
        EmitByte(alignment);

        // Move up to the right alignment for the section data.
        EmitAlignment(alignment);

        // Mark that the section length has been emitted.
        length_emitted = true;
      }
    }

    if (!length_emitted) {
      // Emit section length without alignment constraint.
      EmitVbrInt(shifted_section_length);
    }

    // Then have the payload data.
    EmitBytes(section_data);
  }

  void EmitSection(BEFSectionID section_id, const BefEmitter& emitter) {
    EmitSection(section_id, emitter.result(), emitter.GetRequiredAlignment());
  }
};

constexpr uint32_t BEFFileEmitter::kDummyPseudoKernelCode;
constexpr uint32_t BEFFileEmitter::kDummyPseudoKernelLocation;

}  // namespace

// This is the emitter that builds a BEF into an std::vector.
class BEFModuleEmitter : public BEFFileEmitter {
 public:
  explicit BEFModuleEmitter(mlir::ModuleOp module) : module_(module) {}

  LogicalResult CollectEntities(bool collect_attribute_types_and_names) {
    return entities_.Collect(module_, collect_attribute_types_and_names);
  }

  void EmitLocationInfo();
  void EmitDebugInfo();
  void EmitStrings();
  void EmitAttributes(BEFFileEmitter* attribute_types);
  void EmitKernels();
  void EmitTypes();
  void EmitFunctions(BEFFileEmitter* attribute_names,
                     BEFFileEmitter* register_types);

 private:
  mlir::ModuleOp module_;
  EntityTable entities_;
  EntityIndex entity_index_;
};

void BEFModuleEmitter::EmitLocationInfo() {
  BEFFileEmitter filenames_section;
  for (auto filename : entities_.location_filenames) {
    filenames_section.EmitBytes(
        {reinterpret_cast<const uint8_t*>(filename.data()), filename.size()});
    // Emit a NUL terminator for the filename.
    filenames_section.EmitByte(0);
  }

  EmitSection(BEFSectionID::kLocationFilenames, filenames_section);

  // Emit each of the positions and remember the offsets within the section.
  BEFFileEmitter positions_section;
  for (auto iter : entities_.location_positions) {
    mlir::Operation* op = iter.first;
    auto position = iter.second;
    entity_index_.AddLocationPosition(op, positions_section.size());
    positions_section.EmitVbrInt(std::get<0>(position));
    positions_section.EmitVbrInt(std::get<1>(position));
    positions_section.EmitVbrInt(std::get<2>(position));
  }

  EmitSection(BEFSectionID::kLocationPositions, positions_section);
}

void BEFModuleEmitter::EmitDebugInfo() {
  BEFFileEmitter debug_info_section;

  for (const auto& entry : entities_.debug_info) {
    auto& op = entry.getFirst();
    auto& debug_info = entry.getSecond();

    entity_index_.AddDebugInfoOffset(op, debug_info_section.size());
    debug_info_section.EmitBytes(
        {reinterpret_cast<const uint8_t*>(debug_info.data()),
         debug_info.size()});
    debug_info_section.EmitByte(0);
  }

  EmitSection(BEFSectionID::kDebugInfo, debug_info_section);
}

void BEFModuleEmitter::EmitStrings() {
  // We have an ordered collection of strings: sort them alphabetically to make
  // them stable.
  std::vector<string_view> strs_in_order;
  strs_in_order.reserve(entities_.strings.size());
  for (const auto& str : entities_.strings)
    strs_in_order.push_back(str.getKey());

  std::sort(strs_in_order.begin(), strs_in_order.end());

  // Now that we have all the strings in order, emit them and remember their
  // offsets in the string section.
  BEFFileEmitter string_section;
  for (const auto& entry : strs_in_order) {
    entity_index_.AddString(entry, string_section.size());
    string_section.EmitBytes(
        {reinterpret_cast<const uint8_t*>(entry.data()), entry.size()});
    // Emit a NUL terminator for the string.
    string_section.EmitByte(0);
  }

  EmitSection(BEFSectionID::kStrings, string_section);
}

void BEFModuleEmitter::EmitAttributes(BEFFileEmitter* attribute_types) {
  // The attributes are already in a stable order, so just emit them in the
  // order they were found.

  // Keep track of all compilation units in the module.
  BefCompilationUnits compilation_units(module_);

  // Emit attributes and record them in EntityIndex. Nested array attributes
  // will be traversed recursively and their elements will be emitted and
  // recorded before the top level offsets array is emitted.
  BEFFileEmitter attribute_type_emitter;
  BefAttrEmitter attributes_section;

  for (auto attr : entities_.attributes) {
    auto const attribute_type = BefAttrEmitter::GetBefAttributeType(attr);

    auto const offset =
        (IsSymbolRefAttribute(attribute_type))
            ? attributes_section.EmitSymbolRefAttribute(
                  compilation_units, attr.cast<mlir::SymbolRefAttr>())
            : attributes_section.EmitAttribute(attribute_type, attr);

    entity_index_.AddAttributeOffset(attr, offset);
    if (attribute_types == nullptr) continue;

    const size_t type_info = static_cast<size_t>(attribute_type);
    attribute_type_emitter.EmitVbrInt(offset);
    attribute_type_emitter.EmitVbrInt(type_info);
  }

  if (attribute_types != nullptr) {
    attribute_types->EmitVbrInt(entities_.attributes.size());
    attribute_types->EmitEmitter(attribute_type_emitter);
  }
  EmitSection(BEFSectionID::kAttributes, attributes_section);
}

void BEFModuleEmitter::EmitKernels() {
  // The kernels are already in a stable order, so just emit them in the
  // order they were found.
  BEFFileEmitter ops_section;
  // Count of the number of kernels that exist.
  ops_section.EmitVbrInt(entities_.kernels.size());

  for (auto op : entities_.kernels) {
    auto index = entity_index_.GetStringOffset(op);
    ops_section.EmitVbrInt(index);
  }

  EmitSection(BEFSectionID::kKernels, ops_section);
}

void BEFModuleEmitter::EmitTypes() {
  // The types are already in a stable order, so just emit them in the
  // order they were found.
  BEFFileEmitter types_section;

  // Count of the number of types that exist.
  types_section.EmitVbrInt(entities_.types.size());

  // Emit the index of the name of the types.
  for (auto type : entities_.types) {
    llvm::SmallVector<char, 64> result_str;
    llvm::raw_svector_ostream os(result_str);
    type.print(os);
    auto index = entity_index_.GetStringOffset(os.str());
    types_section.EmitVbrInt(index);
  }

  EmitSection(BEFSectionID::kTypes, types_section);
}

// This is the emitter that builds the function entry of a BEF.
class BEFFunctionEmitter : public BEFFileEmitter {
 public:
  BEFFunctionEmitter(const EntityTable& entities,
                     const EntityIndex& entity_index)
      : entities_(entities), entity_index_(entity_index) {}

  void EmitFunction(mlir::Region* region, BEFFileEmitter* attribute_names,
                    BEFFileEmitter* register_types);

 private:
  void EmitRegisterTable(mlir::Block* block, BEFFileEmitter* register_types);
  template <typename UserRange>
  void EmitKernelResultUsers(UserRange users, BEFFileEmitter* kernel_list,
                             BEFFileEmitter* kernel_body) const;
  void EmitArgumentsPseudoKernel(mlir::Block* block,
                                 BEFFileEmitter* kernel_list) const;
  void EmitKernel(mlir::Operation* op, BEFFileEmitter* kernel_list,
                  BEFFileEmitter* attribute_names) const;

  unsigned GetRegisterNumber(mlir::Value reg) const {
    auto it = register_number_.find(reg);
    assert(it != register_number_.end() && "Unknown register");
    return it->second;
  }

  unsigned GetPseudoResultRegisterNumber() const {
    return register_number_.size();
  }

  void Reset() {
    register_number_.clear();
    kernel_index_.clear();
  }

  llvm::DenseMap<mlir::Value, unsigned> register_number_;
  llvm::DenseMap<mlir::Operation*, unsigned> kernel_index_;

  const EntityTable& entities_;
  const EntityIndex& entity_index_;
};

void BEFFunctionEmitter::EmitFunction(mlir::Region* region,
                                      BEFFileEmitter* attribute_names,
                                      BEFFileEmitter* register_types) {
  Reset();

  assert(llvm::hasSingleElement(*region) && "should have a single block");
  auto& block = region->front();

  auto location_offset =
      entity_index_.GetLocationPositionOffset(region->getParentOp());
  EmitVbrInt(location_offset);

  // Emit the register table.
  EmitRegisterTable(&block, register_types);

  // Get a dense numbering of kernels, including the pseudo kernel.
  unsigned num_kernels = 1;

  for (auto& op : block.getOperations()) {
    if (!IsReturn(&op)) kernel_index_[&op] = num_kernels++;
  }

  // Emit a count of kernels, then the offset of each kernel (from the
  // start of the kernel list) then each kernel is emitted in turn.
  EmitVbrInt(num_kernels);

  mlir::Operation* return_op = nullptr;

  BEFFileEmitter kernel_list;

  if (attribute_names != nullptr) attribute_names->EmitVbrInt(num_kernels);

  // Perform stream analysis to get stream information for this function.
  //
  // TODO(chky): This analysis is better performed at compiler side. However,
  // due to the limitation that asynchrony is implicit at compile-time the only
  // choice to integrate with BEF executor is to perform analysis in MLIRToBEF.
  // Once we make asynchrony explicit at compile-time, we should be able to move
  // this analysis out.
  compiler::StreamAnalysis stream_analysis(block);

  // Before we emit all the kernels, we always emit a pseudo kernel (with no
  // kernel_code) that is the entry to the other kernels. Specifically, its
  // users are:
  //  1) kernels that are using function arguments, and
  //  2) kernels that take no kernel arguments.

  // Offset of the kernel in the list.
  EmitVbrInt(kernel_list.size());
  // Pseudo has zero operands that need to be available.
  EmitVbrInt(0);
  // The pseudo kernel is always in the root stream.
  EmitVbrInt(stream_analysis.GetRootStream().id());

  EmitArgumentsPseudoKernel(&block, &kernel_list);

  for (auto& op : block) {
    // Return kernels get special processing.
    if (IsReturn(&op)) {
      return_op = &op;
      continue;
    }

    bool is_non_strict = false;
    for (auto attr_and_name : op.getAttrs())
      if (BefAttrEmitter::ClassifyAttribute(attr_and_name.first) ==
          SpecialAttribute::kNonStrict) {
        DEBUG_PRINT("This is a non-strict kernel.\n");
        is_non_strict = true;
      }

    // Offset of the kernel in the list.
    EmitVbrInt(kernel_list.size());
    // Number of operands that need to be available before it is ready to go.
    auto num_operands_before_running = op.getNumOperands();

    // We set the number to 1 for non-strict kernels so they get kicked off
    // as soon as any argument is avaiable.  We use 1 instead of zero because we
    // kernels with no operands ready are likely to just wait anyway.
    if (is_non_strict && num_operands_before_running)
      num_operands_before_running = 1;

    EmitVbrInt(num_operands_before_running);

    // Emit stream id from stream analysis.
    const auto& stream = stream_analysis.GetStream(&op);
    EmitVbrInt(stream.id());

    EmitKernel(&op, &kernel_list, attribute_names);
  }

  // Emit the result registers list at the end of the KERNEL_TABLE if present.
  if (return_op) {
    for (auto operand : return_op->getOperands()) {
      EmitVbrInt(GetRegisterNumber(operand));
    }
  }

  // Once we're done, we can emit the kernel data after the kernel index
  // list. Note that kernel entries are fixed32 integers with 4-byte alignment.
  EmitAlignment(4);
  EmitEmitter(kernel_list);

  kernel_index_.clear();
}

void BEFFunctionEmitter::EmitRegisterTable(mlir::Block* block,
                                           BEFFileEmitter* register_types) {
  BEFFileEmitter reg_table;
  BEFFileEmitter reg_type_table;
  unsigned num_registers = 0;

  auto emit_register = [&](mlir::Value reg) {
    // Then the use-count.
    reg_table.EmitVbrInt(std::distance(reg.use_begin(), reg.use_end()));

    // Emit the type index into register types section.
    reg_type_table.EmitVbrInt(entities_.GetTypeIndex(reg.getType()));

    register_number_[reg] = num_registers++;
  };

  for (auto arg : block->getArguments()) emit_register(arg);

  for (auto& op : *block)
    for (auto result : op.getResults()) emit_register(result);

  // Emit the number of registers, then the register table.
  EmitVbrInt(num_registers);
  EmitEmitter(reg_table);

  // Emit the number of registers, then the register type table in register
  // types section.
  if (register_types != nullptr) {
    register_types->EmitVbrInt(num_registers);
    register_types->EmitEmitter(reg_type_table);
  }
}

template <typename UserRange>
void BEFFunctionEmitter::EmitKernelResultUsers(
    UserRange users, BEFFileEmitter* kernel_list,
    BEFFileEmitter* kernel_body) const {
  int num_users = 0;
  for (auto* user : users) {
    // Ignore the 'return' op, it gets special handling.
    if (IsReturn(user)) continue;

    num_users++;
    auto it = kernel_index_.find(user);
    assert(it != kernel_index_.end() && "Invalid user");
    kernel_body->EmitInt4(it->second);
  }
  kernel_list->EmitInt4(num_users);
}

void BEFFunctionEmitter::EmitArgumentsPseudoKernel(
    mlir::Block* block, BEFFileEmitter* kernel_list) const {
  // This kernel starts with a dummy code and a dummy location. And this kernel
  // only has results and used_bys in its body.

  // code
  kernel_list->EmitInt4(kDummyPseudoKernelCode);
  // location
  kernel_list->EmitInt4(kDummyPseudoKernelLocation);
  // arguments
  kernel_list->EmitInt4(0);
  // attributes
  kernel_list->EmitInt4(0);
  // functions
  kernel_list->EmitInt4(0);
  // results, including the special result for ops with no operands.
  kernel_list->EmitInt4(block->getNumArguments() + 1);
  // special_metadata
  kernel_list->EmitInt4(0);

  BEFFileEmitter kernel_body;
  // The first result is the pseudo result used to trigger execution of kernels
  // with no operands.
  kernel_body.EmitInt4(GetPseudoResultRegisterNumber());
  for (auto arg : block->getArguments())
    kernel_body.EmitInt4(GetRegisterNumber(arg));

  // We also emit all operations with no operands as users for the special
  // result.
  llvm::SmallVector<mlir::Operation*, 4> ready_kernels;
  for (auto& op : *block) {
    if (op.getNumOperands() == 0) ready_kernels.push_back(&op);
  }
  EmitKernelResultUsers(ready_kernels, kernel_list, &kernel_body);

  for (auto arg : block->getArguments())
    EmitKernelResultUsers(arg.getUsers(), kernel_list, &kernel_body);

  assert(kernel_list->size() % kKernelEntryAlignment == 0);
  assert(kernel_body.GetRequiredAlignment() == kKernelEntryAlignment);
  kernel_list->EmitEmitter(kernel_body);
}

void BEFFunctionEmitter::EmitKernel(mlir::Operation* op,
                                    BEFFileEmitter* kernel_list,
                                    BEFFileEmitter* attribute_names) const {
  // Each kernel starts out with an opcode record.
  kernel_list->EmitInt4(entities_.GetKernelID(op));

  // Include a location.
  auto location_offset = entity_index_.GetLocationPositionOffset(op);
  kernel_list->EmitInt4(location_offset);

  // Because the numbers of each types of entries are emitted first, we use
  // another emitter to keep all entries and append them to kernel_list later.
  BEFFileEmitter kernel_body;

  // Then we have the arguments.
  kernel_list->EmitInt4(op->getNumOperands());
  for (auto operand : op->getOperands())
    kernel_body.EmitInt4(GetRegisterNumber(operand));

  // Then attributes.
  int num_input_functions = 0;
  int num_input_attributes = 0;
  BEFFileEmitter input_function_emitter;
  BEFFileEmitter input_attribute_emitter;
  uint32_t special_attribute = 0;
  for (auto attr_name_pair : op->getAttrs()) {
    // Skip cost attribute which is not used in runtime execution.
    //
    // TODO(tfrt-devs): Use attribute interface instead of hardcoding here.
    if (attr_name_pair.first == "_tfrt_cost") continue;

    // Emit a flag in kernel header to indicate that the kernel is non-strict.
    if (BefAttrEmitter::ClassifyAttribute(attr_name_pair.first.strref()) ==
        SpecialAttribute::kNonStrict) {
      special_attribute |= static_cast<uint32_t>(SpecialAttribute::kNonStrict);
      continue;
    }

    // Emit array of function attributes.
    if (auto array_fn_attr =
            attr_name_pair.second.dyn_cast<mlir::ArrayAttr>()) {
      if (!array_fn_attr.empty() &&
          array_fn_attr.begin()->dyn_cast<mlir::FlatSymbolRefAttr>()) {
        for (auto fn : array_fn_attr) {
          num_input_functions++;
          input_function_emitter.EmitInt4(entities_.GetFunctionNamed(
              fn.dyn_cast<mlir::FlatSymbolRefAttr>().getValue()));
        }
        continue;
      }
    }

    if (auto fn_attr =
            attr_name_pair.second.dyn_cast<mlir::FlatSymbolRefAttr>()) {
      // Function references are output as regions.
      num_input_functions++;
      input_function_emitter.EmitInt4(
          entities_.GetFunctionNamed(fn_attr.getValue()));
    } else {
      if (attribute_names != nullptr) {
        attribute_names->EmitVbrInt(
            entity_index_.GetStringOffset(attr_name_pair.first));
      }
      num_input_attributes++;

      input_attribute_emitter.EmitInt4(
          entity_index_.GetAttributeOffset(attr_name_pair.second));
    }
  }

  kernel_list->EmitInt4(num_input_attributes);
  kernel_body.EmitEmitter(input_attribute_emitter);

  // Then regions.
  num_input_functions += op->getNumRegions();
  for (auto& region : op->getRegions())
    input_function_emitter.EmitInt4(entities_.GetFunctionID(region));

  kernel_list->EmitInt4(num_input_functions);
  kernel_body.EmitEmitter(input_function_emitter);

  kernel_list->EmitInt4(op->getNumResults());
  for (auto result : op->getResults())
    kernel_body.EmitInt4(GetRegisterNumber(result));

  auto debug_info_offset = entity_index_.GetDebugInfoOffset(op);
  if (debug_info_offset.hasValue()) {
    special_attribute |= static_cast<uint32_t>(SpecialAttribute::kHasDebugInfo);
  }

  // Emit non-strict flag to special_metadata field of kernel header.
  kernel_list->EmitInt4(special_attribute);

  // Then results with the kernels that use them.
  for (auto result : op->getResults())
    EmitKernelResultUsers(result.getUsers(), kernel_list, &kernel_body);

  if (debug_info_offset.hasValue()) {
    kernel_body.EmitInt4(debug_info_offset.getValue());
  }

  assert(kernel_list->size() % kKernelEntryAlignment == 0);
  assert(kernel_body.size() == 0 ||
         kernel_body.GetRequiredAlignment() == kKernelEntryAlignment);
  kernel_list->EmitAlignment(4);
  kernel_list->EmitEmitter(kernel_body);
}

void BEFModuleEmitter::EmitFunctions(BEFFileEmitter* attribute_names,
                                     BEFFileEmitter* register_types) {
  BEFFunctionEmitter functions_section(entities_, entity_index_);

  if (attribute_names != nullptr)
    attribute_names->EmitVbrInt(entities_.functions.size());
  if (register_types != nullptr)
    register_types->EmitVbrInt(entities_.functions.size());
  for (auto function_entry : entities_.functions) {
    // Remember that we emitted this region to this offset.
    entity_index_.AddFunction(function_entry.name, functions_section.size(),
                              function_entry.type, function_entry.kind);
    if (!function_entry.IsNative()) {
      functions_section.EmitFunction(function_entry.region, attribute_names,
                                     register_types);
    }
  }

  // TODO(hyojun): Reduce the increased peak memory usage for keeping
  // function_index_section and functions_section to write the FunctionIndex
  // section before the Functions section.
  // We could improve it by changing the format of FunctionIndex section to
  // use FIXED32 (or FIXED64) instead of VBR integer for function offsets.
  // Or, we could introduce FunctionOffsetTable section, which could be placed
  // after Functions section.
  auto function_index = entity_index_.GetFunctionIndex();
  BEFFileEmitter function_index_section;

  // Count of the number of functions that exist.
  function_index_section.EmitVbrInt(function_index.size());

  for (const auto& entry : function_index) {
    function_index_section.EmitByte(static_cast<uint8_t>(entry.kind));
    function_index_section.EmitVbrInt(entry.function_offset);
    function_index_section.EmitVbrInt(entry.name_offset);

    // Arguments.
    function_index_section.EmitVbrInt(entry.type.getInputs().size());
    for (auto type : entry.type.getInputs())
      function_index_section.EmitVbrInt(entities_.GetTypeIndex(type));

    // Results.
    function_index_section.EmitVbrInt(entry.type.getResults().size());
    for (auto type : entry.type.getResults())
      function_index_section.EmitVbrInt(entities_.GetTypeIndex(type));
  }

  EmitSection(BEFSectionID::kFunctionIndex, function_index_section);
  EmitSection(BEFSectionID::kFunctions, functions_section);
}

// This function converts the specified MLIR module containing a host executor
// compatible program to the BinaryExecutableFormat (BEF) format, which is the
// low level format that the executor takes.
//
// On error, this emits the error message through the MLIR error handler, and
// returns an empty std:vector.
AlignedBuffer<8> ConvertMLIRToBEF(mlir::ModuleOp module,
                                  bool disable_optional_sections) {
  BEFModuleEmitter emitter(module);

  // Build the entities table.
  if (emitter.CollectEntities(!disable_optional_sections) ==
      LogicalResult::Failure)
    return {};

  // Emit magic numbers and format version.
  emitter.EmitBytes({kBEFMagic1, kBEFMagic2, kBEFVersion0});

  BEFFileEmitter attribute_types;
  BEFFileEmitter attribute_names;
  BEFFileEmitter register_types;

  // Emit each section of the file.
  emitter.EmitLocationInfo();
  emitter.EmitDebugInfo();
  emitter.EmitStrings();
  emitter.EmitAttributes(disable_optional_sections ? nullptr
                                                   : &attribute_types);
  emitter.EmitKernels();
  emitter.EmitTypes();

  if (disable_optional_sections) {
    emitter.EmitFunctions(/*attribute_names=*/nullptr,
                          /*register_types=*/nullptr);
  } else {
    emitter.EmitFunctions(&attribute_names, &register_types);

    emitter.EmitSection(BEFSectionID::kAttributeTypes, attribute_types);
    emitter.EmitSection(BEFSectionID::kAttributeNames, attribute_names);
    emitter.EmitSection(BEFSectionID::kRegisterTypes, register_types);
  }

  // Return the result.
  return emitter.TakeResult();
}

}  // namespace tfrt
