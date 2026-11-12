// Nodes made from a game's Lua API definitions: the LuaLS (LuaCATS) files a recomp ships beside its executable in
// lua_api, each starting with ---@meta. A function makes a call node, an event overload (`fun(event: "name", fn:
// fun(...))`) an event node and a method a node taking its object, so a game's own Lua wrappers become nodes
// without it declaring anything to RexFabric.

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform/file.h>

#include "lua_compiler.h"
#include "node_library.h"

namespace rexfabric::graph {
namespace {

std::string Trim(std::string_view text) {
  size_t a = 0, b = text.size();
  while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
  return std::string(text.substr(a, b - a));
}

// The type that starts `text`: up to the first space outside brackets and quotes. `rest` gets what follows.
std::string TakeType(std::string_view text, std::string* rest) {
  int depth = 0;
  bool quoted = false;
  size_t i = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"') quoted = !quoted;
    if (quoted) continue;
    if (c == '(' || c == '{' || c == '<' || c == '[') ++depth;
    if (c == ')' || c == '}' || c == '>' || c == ']') --depth;
    // "fun(a: x): y" keeps its return type.
    if (std::isspace(static_cast<unsigned char>(c)) && depth == 0 && !(i > 0 && text[i - 1] == ':')) break;
  }
  if (rest) *rest = Trim(text.substr(i));
  return std::string(text.substr(0, i));
}

// `text` split at top-level commas.
std::vector<std::string> SplitTop(std::string_view text) {
  std::vector<std::string> parts;
  int depth = 0;
  bool quoted = false;
  size_t start = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"') quoted = !quoted;
    if (quoted) continue;
    if (c == '(' || c == '{' || c == '<' || c == '[') ++depth;
    if (c == ')' || c == '}' || c == '>' || c == ']') --depth;
    if (c == ',' && depth == 0) {
      parts.push_back(Trim(text.substr(start, i - start)));
      start = i + 1;
    }
  }
  if (const std::string last = Trim(text.substr(start)); !last.empty()) parts.push_back(last);
  return parts;
}

struct Named {
  std::string name;
  std::string type;
  bool optional = false;
  std::string description;
};

// "fun(part: renut.Part, dt: number): number" as its parameters.
std::optional<std::vector<Named>> FunParams(std::string_view type) {
  if (!type.starts_with("fun(")) return std::nullopt;
  int depth = 0;
  size_t close = std::string_view::npos;
  for (size_t i = 3; i < type.size(); ++i) {
    if (type[i] == '(') ++depth;
    if (type[i] == ')' && --depth == 0) {
      close = i;
      break;
    }
  }
  if (close == std::string_view::npos) return std::nullopt;
  std::vector<Named> params;
  for (const std::string& part : SplitTop(type.substr(4, close - 4))) {
    const size_t colon = part.find(':');
    Named param;
    param.name = Trim(colon == std::string::npos ? part : part.substr(0, colon));
    param.type = colon == std::string::npos ? "any" : Trim(part.substr(colon + 1));
    if (param.name.ends_with('?')) {
      param.name.pop_back();
      param.optional = true;
    }
    params.push_back(param);
  }
  return params;
}

struct ClassDef {
  std::vector<Named> fields;
  bool methods = false;  // an object with methods, rather than a table of values
  // ---@operator add(renut.Vec): renut.Vec; a unary one (unm) has no operand.
  struct Operator {
    std::string name, operand, result;
  };
  std::vector<Operator> operators;
};

struct FunctionDef {
  std::string name;   // "renut.parts.add", or "renut.Vec:length" for a method
  std::string self;   // a method's class
  std::string description;
  std::vector<Named> params;
  std::vector<Named> returns;
  std::vector<std::string> overloads;
  bool pure = false;  // ---@nodiscard: a value, worked out wherever it's read
};

struct Api {
  std::map<std::string, ClassDef> classes;
  std::map<std::string, std::string> aliases;            // name -> its type, or the literals it's one of
  std::map<std::string, std::vector<std::string>> literals;  // alias -> the strings it's one of
  // Aliases of strings that can be something else too (renut.Place: a vector, "front" or "banjo").
  std::set<std::string> open;
  std::vector<FunctionDef> functions;
};

// An alias's members that aren't string literals or nil: a vector or a list beside its strings.
bool HasOtherMembers(std::string_view type) {
  int depth = 0;
  bool quoted = false;
  size_t start = 0;
  for (size_t i = 0; i <= type.size(); ++i) {
    const char c = i < type.size() ? type[i] : '|';
    if (c == '"') quoted = !quoted;
    if (quoted) continue;
    if (c == '(' || c == '{' || c == '<' || c == '[') ++depth;
    if (c == ')' || c == '}' || c == '>' || c == ']') --depth;
    if (c != '|' || depth != 0) continue;
    const std::string member = Trim(type.substr(start, i - start));
    if (!member.empty() && !member.starts_with('"') && member != "nil") return true;
    start = i + 1;
  }
  return false;
}

// Reads one definition file into `api`.
void ReadFile(const std::filesystem::path& file, Api& api) {
  std::ifstream in(file);
  std::string line;
  if (!std::getline(in, line) || Trim(line) != "---@meta") return;
  FunctionDef doc;
  std::string current_class, current_alias;
  std::map<std::string, std::string> locals;  // local table -> the class it is
  std::string pending_class;                  // the last ---@class, for `local X = {}` after it
  auto reset = [&] {
    doc = {};
    current_class.clear();
    current_alias.clear();
  };
  while (std::getline(in, line)) {
    const std::string text = Trim(line);
    if (text.starts_with("---|")) {
      // An alias's next literal: ---| "name" description
      std::string value = Trim(std::string_view(text).substr(4));
      if (!current_alias.empty() && value.starts_with('"')) {
        const size_t end = value.find('"', 1);
        if (end != std::string::npos) api.literals[current_alias].push_back(value.substr(1, end - 1));
      } else if (!current_alias.empty() && !value.empty()) {
        api.open.insert(current_alias);
      }
      continue;
    }
    if (text.starts_with("---@")) {
      std::string rest;
      const std::string tag = TakeType(std::string_view(text).substr(4), &rest);
      if (tag == "class") {
        current_class = TakeType(rest, nullptr);
        if (const size_t colon = current_class.find(':'); colon != std::string::npos) current_class = Trim(current_class.substr(0, colon));
        api.classes[current_class];
        pending_class = current_class;
      } else if (tag == "field" && !current_class.empty()) {
        Named field;
        std::string after;
        field.name = TakeType(rest, &after);
        if (field.name.ends_with('?')) {
          field.name.pop_back();
          field.optional = true;
        }
        field.type = TakeType(after, &field.description);
        api.classes[current_class].fields.push_back(field);
      } else if (tag == "alias") {
        std::string after;
        current_alias = TakeType(rest, &after);
        api.aliases[current_alias] = after;
        if (after.find('"') != std::string::npos && HasOtherMembers(TakeType(after, nullptr))) api.open.insert(current_alias);
        // ---@alias X "a"|"b"
        size_t at = 0;
        while ((at = after.find('"', at)) != std::string::npos) {
          const size_t end = after.find('"', at + 1);
          if (end == std::string::npos) break;
          api.literals[current_alias].push_back(after.substr(at + 1, end - at - 1));
          at = end + 1;
        }
      } else if (tag == "param") {
        Named param;
        std::string after;
        param.name = TakeType(rest, &after);
        if (param.name.ends_with('?')) {
          param.name.pop_back();
          param.optional = true;
        }
        param.type = TakeType(after, &param.description);
        doc.params.push_back(param);
      } else if (tag == "return") {
        Named result;
        std::string after;
        result.type = TakeType(rest, &after);
        // ---@return type name description: a name is one word.
        std::string name = TakeType(after, &result.description);
        if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; })) result.name = name;
        else result.description = after;
        doc.returns.push_back(result);
      } else if (tag == "overload") {
        doc.overloads.push_back(rest);
      } else if (tag == "nodiscard") {
        doc.pure = true;
      } else if (tag == "operator" && !current_class.empty()) {
        // add(renut.Vec): renut.Vec, or unm: renut.Vec
        const std::string spec = TakeType(rest, nullptr);
        const size_t colon = spec.rfind(':');
        std::string head = Trim(colon == std::string::npos ? spec : spec.substr(0, colon));
        ClassDef::Operator op;
        op.result = colon == std::string::npos ? current_class : Trim(spec.substr(colon + 1));
        if (const size_t open = head.find('('); open != std::string::npos) {
          const size_t close = head.rfind(')');
          op.operand = Trim(head.substr(open + 1, (close == std::string::npos ? head.size() : close) - open - 1));
          head = Trim(head.substr(0, open));
        }
        op.name = head;
        api.classes[current_class].operators.push_back(op);
      }
      continue;
    }
    if (text.starts_with("---")) {
      const std::string said = Trim(std::string_view(text).substr(3));
      doc.description += (doc.description.empty() ? "" : " ") + said;
      continue;
    }
    // local Vec = {}: the methods of `Vec` are the last class's.
    if (text.starts_with("local ") && text.ends_with("= {}") && !pending_class.empty()) {
      const std::string local = Trim(std::string_view(text).substr(6, text.size() - 6 - 4));
      locals[local] = pending_class;
    }
    if (text.starts_with("function ")) {
      const size_t open = text.find('(');
      const size_t close = text.find(')', open == std::string::npos ? 0 : open);
      if (open != std::string::npos && close != std::string::npos) {
        FunctionDef def = doc;
        def.name = Trim(std::string_view(text).substr(9, open - 9));
        if (const size_t colon = def.name.find(':'); colon != std::string::npos) {
          const std::string owner = def.name.substr(0, colon);
          def.self = locals.count(owner) ? locals[owner] : owner;
          def.name = def.self + ":" + def.name.substr(colon + 1);
          api.classes[def.self].methods = true;
        }
        // Parameters the comments left out are any.
        std::vector<Named> params;
        for (const std::string& name : SplitTop(std::string_view(text).substr(open + 1, close - open - 1))) {
          if (name.empty() || name == "...") continue;
          const auto documented = std::find_if(def.params.begin(), def.params.end(), [&](const Named& p) { return p.name == name; });
          params.push_back(documented != def.params.end() ? *documented : Named{name, "any", true, {}});
        }
        def.params = params;
        api.functions.push_back(def);
      }
    }
    reset();
  }
}

// The strings an input of `type` is picked from, made once for the library's nodes to keep.
const ChoiceSource* Choices(const Api& api, const std::string& type) {
  static std::deque<ChoiceSource> made;
  std::vector<std::string> values;
  if (auto alias = api.literals.find(type); alias != api.literals.end()) values = alias->second;
  if (values.empty() && type.find('"') != std::string::npos) {
    size_t at = 0;
    while ((at = type.find('"', at)) != std::string::npos) {
      const size_t end = type.find('"', at + 1);
      if (end == std::string::npos) break;
      values.push_back(type.substr(at + 1, end - at - 1));
      at = end + 1;
    }
  }
  if (values.empty()) return nullptr;
  ChoiceSource& source = made.emplace_back();
  for (const std::string& value : values) source.fixed.push_back({value, value});
  return &source;
}

std::string Base(std::string type) {
  if (type.ends_with('?')) type.pop_back();
  // "T|nil" is T, maybe left out.
  if (type.ends_with("|nil")) type.resize(type.size() - 4);
  return type;
}

bool IsTable(const Api& api, const std::string& type) {
  const auto found = api.classes.find(Base(type));
  return found != api.classes.end() && !found->second.methods && !found->second.fields.empty();
}

// A pin for a value of `type`.
Pin PinFor(const Api& api, const std::string& name, const std::string& raw, bool optional) {
  const std::string type = Base(raw);
  std::string resolved = type;
  if (auto alias = api.aliases.find(type); alias != api.aliases.end() && !api.literals.count(type)) resolved = Base(alias->second);
  // A union's first member when that's an object: renut.Vec|{x, y, z} takes a renut.Vec.
  if (const size_t bar = resolved.find('|'); bar != std::string::npos) {
    const std::string first = Trim(std::string_view(resolved).substr(0, bar));
    if (auto found = api.classes.find(first); found != api.classes.end() && found->second.methods) resolved = first;
  }
  Pin pin;
  if (resolved == "number") pin = DataPin(name, PinKind::kFloat);
  else if (resolved == "integer") pin = DataPin(name, PinKind::kInt);
  else if (resolved == "string") pin = DataPin(name, PinKind::kString);
  else if (resolved == "boolean") pin = DataPin(name, PinKind::kBool);
  else if (const ChoiceSource* choices = Choices(api, type)) {
    // Strings to pick from; an alias that can be something else too takes anything linked to it.
    pin = DataPin(name, api.open.count(type) || HasOtherMembers(type) ? PinKind::kAny : PinKind::kString, choices->fixed.front().second);
    pin.choices = choices;
  } else if (auto found = api.classes.find(resolved); found != api.classes.end() && found->second.methods) {
    pin = ObjectPin(name, resolved);
  } else {
    pin = DataPin(name, PinKind::kAny);
  }
  if (optional || raw.ends_with('?') || raw.ends_with("|nil")) pin.required = false;
  return pin;
}

// "renut.parts.add" under "API/renut/parts"; a method under its class.
std::string CategoryOf(const FunctionDef& def) {
  if (!def.self.empty()) {
    std::string path = def.self;
    for (char& c : path) if (c == '.') c = '/';
    return "API/" + path;
  }
  const size_t last = def.name.rfind('.');
  std::string path = last == std::string::npos ? std::string("global") : def.name.substr(0, last);
  for (char& c : path) if (c == '.') c = '/';
  return "API/" + path;
}

std::string LastName(const std::string& name) {
  const size_t at = name.find_last_of(".:");
  return at == std::string::npos ? name : name.substr(at + 1);
}

std::string Joined(const std::vector<std::string>& parts) {
  std::string joined;
  for (size_t i = 0; i < parts.size(); ++i) joined += (i ? ", " : "") + parts[i];
  return joined;
}

// A callback a call takes: an exec output whose chain is the callback's body, and its parameters as outputs.
struct Callback {
  std::string key;  // its field in a table argument, or empty for an argument of its own
  std::string pin;  // the exec output
  std::vector<std::string> params;  // the outputs of its parameters
};

// A call's argument: plain inputs, or a table of fields and callbacks.
struct Argument {
  std::string pin;  // a plain argument's input
  bool table = false;
  std::vector<std::pair<std::string, std::string>> fields;  // field, input
  std::vector<bool> optional;                               // per field
  std::vector<Callback> callbacks;                          // in a table, or the argument itself
};

// A call's arguments, as Lua: plain inputs, and tables of their fields (pure calls have no callbacks).
std::string PlainArguments(Emitter& emit, const std::vector<Argument>& arguments) {
  std::vector<std::string> args;
  for (const Argument& arg : arguments) {
    if (!arg.table) {
      args.push_back(emit.Input(arg.pin));
      continue;
    }
    std::vector<std::string> fields;
    for (size_t f = 0; f < arg.fields.size(); ++f) {
      const auto& [key, pin] = arg.fields[f];
      if (arg.optional[f] && !emit.Linked(pin) && !emit.node().values.count(pin)) continue;
      fields.push_back(key + " = " + emit.Input(pin));
    }
    args.push_back("{" + Joined(fields) + "}");
  }
  return Joined(args);
}

// A ---@nodiscard function as a pure node, as Unreal's pure functions: no exec pins, its value worked out wherever
// it's read. False when it takes a callback, which needs a chain.
bool AddPure(Library& library, const Api& api, const FunctionDef& def) {
  if (def.returns.empty()) return false;
  NodeDesc desc{.id = "api.pure:" + def.name, .title = LastName(def.name), .category = CategoryOf(def), .description = def.description, .role = NodeRole::kPure};
  if (!def.self.empty()) desc.pins.inputs.push_back(ObjectPin("Self", def.self));
  auto arguments = std::make_shared<std::vector<Argument>>();
  for (const Named& param : def.params) {
    if (FunParams(param.type)) return false;
    Argument arg;
    if (IsTable(api, param.type)) {
      arg.table = true;
      for (const Named& field : api.classes.at(Base(param.type)).fields) {
        if (FunParams(field.type)) return false;
        desc.pins.inputs.push_back(PinFor(api, field.name, field.type, field.optional));
        arg.fields.push_back({field.name, field.name});
        arg.optional.push_back(field.optional);
      }
    } else {
      desc.pins.inputs.push_back(PinFor(api, param.name, param.type, param.optional));
      arg.pin = param.name;
    }
    arguments->push_back(arg);
  }
  std::vector<std::string> results;
  for (size_t i = 0; i < def.returns.size(); ++i) {
    const Named& result = def.returns[i];
    results.push_back(result.name.empty() ? (i ? "Result " + std::to_string(i + 1) : std::string("Result")) : result.name);
    desc.pins.outputs.push_back(PinFor(api, results.back(), result.type, true));
  }
  const std::string callee = def.self.empty() ? def.name : ":" + LastName(def.name);
  desc.expression = [arguments, results, callee](Emitter& emit, std::string_view output) -> std::string {
    const std::string call = (callee.starts_with(':') ? "(" + emit.Input("Self") + ")" + callee : callee) + "(" + PlainArguments(emit, *arguments) + ")";
    if (results.size() <= 1) return call;
    const auto found = std::find(results.begin(), results.end(), output);
    return "(select(" + std::to_string(found - results.begin() + 1) + ", " + call + "))";
  };
  library.Add(std::move(desc));
  return true;
}

// "renut.Vec" under "API/renut/Vec", with its methods.
std::string ClassCategory(std::string name) {
  for (char& c : name) if (c == '.') c = '/';
  return "API/" + name;
}

// ---@operator lines of a class: a pure node for each, A + B or -A.
void AddOperators(Library& library, const Api& api) {
  struct Symbol {
    const char* lua;
    const char* word;
  };
  static const std::map<std::string, Symbol> kSymbols = {{"add", {"+", "Add"}}, {"sub", {"-", "Subtract"}}, {"mul", {"*", "Multiply"}},
                                                         {"div", {"/", "Divide"}}, {"mod", {"%", "Modulo"}}, {"pow", {"^", "Power"}},
                                                         {"idiv", {"//", "Floor Divide"}}, {"concat", {"..", "Concat"}}, {"unm", {"-", "Negate"}}};
  const auto operandTitle = [](const std::string& type) {
    const std::string base = Base(type);
    if (base == "number" || base == "integer") return std::string("Number");
    return LastName(base);
  };
  for (const auto& [name, def] : api.classes) {
    for (const ClassDef::Operator& op : def.operators) {
      const auto symbol = kSymbols.find(op.name);
      if (symbol == kSymbols.end()) continue;
      const bool unary = op.operand.empty();
      const std::string self = LastName(name);
      const std::string lua = symbol->second.lua;
      NodeDesc desc{.id = "api.op:" + name + ":" + op.name + (unary ? std::string() : ":" + Base(op.operand)),
                    .title = unary ? std::string(symbol->second.word) + " " + self : self + " " + lua + " " + operandTitle(op.operand),
                    .category = ClassCategory(name),
                    .description = unary ? lua + "A." : "A " + lua + " B.",
                    .keywords = lua + " " + symbol->second.word + " operator",
                    .role = NodeRole::kPure};
      if (library.Find(desc.id)) continue;
      desc.pins.inputs.push_back(PinFor(api, "A", name, false));
      if (!unary) desc.pins.inputs.push_back(PinFor(api, "B", op.operand, false));
      desc.pins.outputs.push_back(PinFor(api, "Result", op.result, true));
      desc.expression = [unary, lua](Emitter& emit, std::string_view) -> std::string {
        return unary ? "(" + lua + " " + emit.Input("A") + ")" : "(" + emit.Input("A") + " " + lua + " " + emit.Input("B") + ")";
      };
      library.Add(std::move(desc));
    }
  }
}

// An object class's fields (renut.Vec's x, y and z) as the outputs of one pure node, as Unreal's Break.
void AddBreaks(Library& library, const Api& api) {
  for (const auto& [name, def] : api.classes) {
    if (!def.methods || def.fields.empty()) continue;
    NodeDesc desc{.id = "api.break:" + name, .title = "Break " + LastName(name),
                  .category = ClassCategory(name),
                  .description = "The " + LastName(name) + "'s fields.", .keywords = "split fields members", .role = NodeRole::kPure};
    if (library.Find(desc.id)) continue;
    desc.pins.inputs.push_back(PinFor(api, "Self", name, false));
    for (const Named& field : def.fields) desc.pins.outputs.push_back(PinFor(api, field.name, field.type, true));
    desc.expression = [](Emitter& emit, std::string_view output) -> std::string { return "(" + emit.Input("Self") + ")" + LuaMember(output); };
    library.Add(std::move(desc));
  }
}

// `hidden`: left out of the add menu, for a function that has a pure node; graphs already using the call still load.
void AddCall(Library& library, const Api& api, const FunctionDef& def, bool hidden) {
  NodeDesc desc{.id = "api.call:" + def.name, .title = LastName(def.name), .category = hidden ? std::string() : CategoryOf(def), .description = def.description};
  desc.pins.inputs.push_back(ExecPin());
  desc.pins.outputs.push_back(ExecPin(kExecOut));
  if (!def.self.empty()) desc.pins.inputs.push_back(ObjectPin("Self", def.self));
  auto arguments = std::make_shared<std::vector<Argument>>();
  for (const Named& param : def.params) {
    Argument arg;
    if (IsTable(api, param.type)) {
      arg.table = true;
      for (const Named& field : api.classes.at(Base(param.type)).fields) {
        if (const auto fun = FunParams(field.type)) {
          Callback callback{field.name, field.name, {}};
          desc.pins.outputs.push_back(ExecPin(field.name));
          for (const Named& p : *fun) {
            callback.params.push_back(field.name + " " + p.name);
            desc.pins.outputs.push_back(PinFor(api, callback.params.back(), p.type, true));
          }
          arg.callbacks.push_back(callback);
          continue;
        }
        desc.pins.inputs.push_back(PinFor(api, field.name, field.type, field.optional));
        arg.fields.push_back({field.name, field.name});
        arg.optional.push_back(field.optional);
      }
    } else if (const auto fun = FunParams(param.type)) {
      Callback callback{{}, param.name, {}};
      desc.pins.outputs.push_back(ExecPin(param.name));
      for (const Named& p : *fun) {
        callback.params.push_back(param.name + " " + p.name);
        desc.pins.outputs.push_back(PinFor(api, callback.params.back(), p.type, true));
      }
      arg.callbacks.push_back(callback);
    } else {
      desc.pins.inputs.push_back(PinFor(api, param.name, param.type, param.optional));
      arg.pin = param.name;
    }
    arguments->push_back(arg);
  }
  std::vector<std::string> results;
  for (size_t i = 0; i < def.returns.size(); ++i) {
    const Named& result = def.returns[i];
    std::string name = result.name.empty() ? (i ? "Result " + std::to_string(i + 1) : std::string("Result")) : result.name;
    results.push_back(name);
    desc.pins.outputs.push_back(PinFor(api, name, result.type, true));
  }
  const std::string callee = def.self.empty() ? def.name : ":" + LastName(def.name);
  desc.emit = [arguments, results, callee](Emitter& emit) {
    std::vector<std::string> outs;
    for (const std::string& result : results) outs.push_back(emit.Output(result));
    // Callback parameters are declared out here, so in the callback they're its parameters.
    for (const Argument& arg : *arguments)
      for (const Callback& callback : arg.callbacks)
        for (const std::string& p : callback.params) emit.Output(p);
    std::string line = (outs.empty() ? std::string() : Joined(outs) + " = ") +
                       (callee.starts_with(':') ? emit.Input("Self") + callee : callee) + "(";
    auto callbackHead = [&](const Callback& callback) {
      std::vector<std::string> params;
      for (const std::string& p : callback.params) params.push_back(emit.Output(p));
      return "function(" + Joined(params) + ")";
    };
    auto writeCallback = [&](const Callback& callback) {
      emit.Open();
      emit.Chain(callback.pin);
      emit.Close();
    };
    for (size_t i = 0; i < arguments->size(); ++i) {
      const Argument& arg = (*arguments)[i];
      if (i) line += ", ";
      if (!arg.table && arg.callbacks.empty()) {
        line += emit.Input(arg.pin);
        continue;
      }
      if (!arg.table) {
        const Callback& callback = arg.callbacks.front();
        if (!emit.Linked(callback.pin)) {
          line += "nil";
          continue;
        }
        emit.Line(line + callbackHead(callback));
        writeCallback(callback);
        line = "end";
        continue;
      }
      emit.Line(line + "{");
      emit.Indent();
      for (size_t f = 0; f < arg.fields.size(); ++f) {
        const auto& [key, pin] = arg.fields[f];
        // A field left unlinked with nothing typed in is left out, so the function's own default stays.
        if (arg.optional[f] && !emit.Linked(pin) && !emit.node().values.count(pin)) continue;
        emit.Line(key + " = " + emit.Input(pin) + ",");
      }
      for (const Callback& callback : arg.callbacks) {
        if (!emit.Linked(callback.pin)) continue;
        emit.Line(callback.key + " = " + callbackHead(callback));
        writeCallback(callback);
        emit.Line("end,");
      }
      emit.Dedent();
      line = "}";
    }
    emit.Line(line + ")");
    emit.Chain(kExecOut);
  };
  library.Add(std::move(desc));
}

// renut.parts.on(event, fn) with ---@overload fun(event: "damage", fn: fun(part, amount)): an event node for each.
bool AddEvents(Library& library, const Api& api, const FunctionDef& def) {
  bool any = false;
  for (const std::string& overload : def.overloads) {
    const auto params = FunParams(overload);
    if (!params || params->size() != 2) continue;
    const Named& event = (*params)[0];
    const auto fn = FunParams((*params)[1].type);
    if (!event.type.starts_with('"') || !fn) continue;
    const std::string name = event.type.substr(1, event.type.find('"', 1) - 1);
    NodeDesc desc{.id = "api.event:" + def.name + ":" + name, .title = "On " + name, .category = CategoryOf(def),
                  .description = def.description, .role = NodeRole::kEvent};
    desc.pins.outputs.push_back(ExecPin(kExecOut));
    std::vector<std::string> outs;
    for (const Named& p : *fn) {
      outs.push_back(p.name);
      desc.pins.outputs.push_back(PinFor(api, p.name, p.type, true));
    }
    const std::string callee = def.name;
    desc.emit = [callee, name, outs](Emitter& emit) {
      std::vector<std::string> params;
      for (const std::string& out : outs) params.push_back(emit.Param(out));
      emit.Line(callee + "(" + LuaQuote(name) + ", function(" + Joined(params) + ")");
      emit.Open();
      emit.Chain(kExecOut);
      emit.Close();
      emit.Line("end)");
    };
    library.Add(std::move(desc));
    any = true;
  }
  return any;
}

}  // namespace

void AddApiNodes(Library& library) {
  Api api;
  std::error_code error;
  const std::filesystem::path folder = rex::platform::GetExecutableFolder() / "lua_api";
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (entry.is_regular_file(error) && entry.path().extension() == ".lua") ReadFile(entry.path(), api);
  }
  for (const FunctionDef& def : api.functions) {
    if (library.Find("api.call:" + def.name)) continue;
    if (AddEvents(library, api, def)) continue;
    const bool pure = def.pure && AddPure(library, api, def);
    AddCall(library, api, def, pure);
  }
  AddOperators(library, api);
  AddBreaks(library, api);
}

}  // namespace rexfabric::graph
