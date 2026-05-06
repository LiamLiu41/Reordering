// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_cc_profile.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

struct JsonValue {
    enum class Type {
        NIL,
        BOOL,
        NUMBER,
        STRING,
        OBJECT,
    };

    Type type = Type::NIL;
    bool bool_value = false;
    double number_value = 0.0;
    std::string string_value;
    std::unordered_map<std::string, std::shared_ptr<JsonValue>> object_value;

    bool isObject() const { return type == Type::OBJECT; }
    bool isString() const { return type == Type::STRING; }
    bool isNumber() const { return type == Type::NUMBER; }
    bool isBool() const { return type == Type::BOOL; }

    const JsonValue* find(const std::string& key) const {
        if (!isObject()) {
            return nullptr;
        }
        auto it = object_value.find(key);
        if (it == object_value.end()) {
            return nullptr;
        }
        return it->second.get();
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : _input(input) {}

    bool parse(JsonValue* out, std::string* error) {
        skipWs();
        if (!parseValue(out, error)) {
            return false;
        }
        skipWs();
        if (_pos != _input.size()) {
            return setError(error, "unexpected trailing characters in JSON");
        }
        return true;
    }

private:
    bool setError(std::string* error, const std::string& msg) const {
        if (error) {
            std::ostringstream oss;
            oss << msg << " at offset " << _pos;
            *error = oss.str();
        }
        return false;
    }

    void skipWs() {
        while (_pos < _input.size() && std::isspace(static_cast<unsigned char>(_input[_pos]))) {
            _pos++;
        }
    }

    bool parseValue(JsonValue* out, std::string* error) {
        if (_pos >= _input.size()) {
            return setError(error, "unexpected end of JSON");
        }
        char c = _input[_pos];
        if (c == '{') {
            return parseObject(out, error);
        }
        if (c == '"') {
            out->type = JsonValue::Type::STRING;
            return parseString(&out->string_value, error);
        }
        if (c == 't' || c == 'f') {
            return parseBool(out, error);
        }
        if (c == 'n') {
            return parseNull(out, error);
        }
        if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) {
            out->type = JsonValue::Type::NUMBER;
            return parseNumber(&out->number_value, error);
        }
        return setError(error, "unsupported JSON value");
    }

    bool parseObject(JsonValue* out, std::string* error) {
        if (_input[_pos] != '{') {
            return setError(error, "expected '{'");
        }
        out->type = JsonValue::Type::OBJECT;
        out->object_value.clear();
        _pos++;
        skipWs();

        if (_pos < _input.size() && _input[_pos] == '}') {
            _pos++;
            return true;
        }

        while (_pos < _input.size()) {
            std::string key;
            if (!parseString(&key, error)) {
                return false;
            }
            skipWs();
            if (_pos >= _input.size() || _input[_pos] != ':') {
                return setError(error, "expected ':' after object key");
            }
            _pos++;
            skipWs();

            JsonValue value;
            if (!parseValue(&value, error)) {
                return false;
            }
            out->object_value[key] = std::make_shared<JsonValue>(value);

            skipWs();
            if (_pos >= _input.size()) {
                return setError(error, "unexpected end while parsing object");
            }
            if (_input[_pos] == '}') {
                _pos++;
                return true;
            }
            if (_input[_pos] != ',') {
                return setError(error, "expected ',' or '}' in object");
            }
            _pos++;
            skipWs();
        }

        return setError(error, "unterminated object");
    }

    bool parseString(std::string* out, std::string* error) {
        if (_pos >= _input.size() || _input[_pos] != '"') {
            return setError(error, "expected string");
        }
        _pos++;
        out->clear();

        while (_pos < _input.size()) {
            char c = _input[_pos++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (_pos >= _input.size()) {
                    return setError(error, "unterminated escape in string");
                }
                char esc = _input[_pos++];
                switch (esc) {
                    case '"': out->push_back('"'); break;
                    case '\\': out->push_back('\\'); break;
                    case '/': out->push_back('/'); break;
                    case 'b': out->push_back('\b'); break;
                    case 'f': out->push_back('\f'); break;
                    case 'n': out->push_back('\n'); break;
                    case 'r': out->push_back('\r'); break;
                    case 't': out->push_back('\t'); break;
                    default:
                        return setError(error, "unsupported string escape");
                }
            } else {
                out->push_back(c);
            }
        }

        return setError(error, "unterminated string");
    }

    bool parseNumber(double* out, std::string* error) {
        const char* begin = _input.c_str() + _pos;
        char* end = nullptr;
        errno = 0;
        double value = std::strtod(begin, &end);
        if (begin == end) {
            return setError(error, "invalid number");
        }
        if (errno == ERANGE) {
            return setError(error, "number out of range");
        }
        _pos += static_cast<size_t>(end - begin);
        *out = value;
        return true;
    }

    bool parseBool(JsonValue* out, std::string* error) {
        if (_input.compare(_pos, 4, "true") == 0) {
            out->type = JsonValue::Type::BOOL;
            out->bool_value = true;
            _pos += 4;
            return true;
        }
        if (_input.compare(_pos, 5, "false") == 0) {
            out->type = JsonValue::Type::BOOL;
            out->bool_value = false;
            _pos += 5;
            return true;
        }
        return setError(error, "invalid boolean literal");
    }

    bool parseNull(JsonValue* out, std::string* error) {
        if (_input.compare(_pos, 4, "null") != 0) {
            return setError(error, "invalid null literal");
        }
        out->type = JsonValue::Type::NIL;
        _pos += 4;
        return true;
    }

private:
    const std::string& _input;
    size_t _pos = 0;
};

class ExprParser {
public:
    ExprParser(const std::string& expr,
               const std::function<std::optional<double>(const std::string&)>& var_resolver)
        : _expr(expr), _var_resolver(var_resolver) {}

    bool parse(double* out, std::string* error) {
        skipWs();
        if (!parseExpr(out, error)) {
            return false;
        }
        skipWs();
        if (_pos != _expr.size()) {
            return setError(error, "unexpected trailing tokens in expression");
        }
        return true;
    }

private:
    bool setError(std::string* error, const std::string& msg) const {
        if (error) {
            std::ostringstream oss;
            oss << msg << " at expr offset " << _pos;
            *error = oss.str();
        }
        return false;
    }

    void skipWs() {
        while (_pos < _expr.size() && std::isspace(static_cast<unsigned char>(_expr[_pos]))) {
            _pos++;
        }
    }

    bool parseExpr(double* out, std::string* error) {
        if (!parseTerm(out, error)) {
            return false;
        }
        while (true) {
            skipWs();
            if (_pos >= _expr.size() || (_expr[_pos] != '+' && _expr[_pos] != '-')) {
                break;
            }
            char op = _expr[_pos++];
            double rhs = 0.0;
            if (!parseTerm(&rhs, error)) {
                return false;
            }
            if (op == '+') {
                *out += rhs;
            } else {
                *out -= rhs;
            }
        }
        return true;
    }

    bool parseTerm(double* out, std::string* error) {
        if (!parseFactor(out, error)) {
            return false;
        }
        while (true) {
            skipWs();
            if (_pos >= _expr.size() || (_expr[_pos] != '*' && _expr[_pos] != '/')) {
                break;
            }
            char op = _expr[_pos++];
            double rhs = 0.0;
            if (!parseFactor(&rhs, error)) {
                return false;
            }
            if (op == '*') {
                *out *= rhs;
            } else {
                if (rhs == 0.0) {
                    return setError(error, "division by zero");
                }
                *out /= rhs;
            }
        }
        return true;
    }

    bool parseFactor(double* out, std::string* error) {
        skipWs();
        if (_pos >= _expr.size()) {
            return setError(error, "unexpected end of expression");
        }

        if (_expr[_pos] == '(') {
            _pos++;
            if (!parseExpr(out, error)) {
                return false;
            }
            skipWs();
            if (_pos >= _expr.size() || _expr[_pos] != ')') {
                return setError(error, "expected ')' in expression");
            }
            _pos++;
            return true;
        }

        if (_expr[_pos] == '+' || _expr[_pos] == '-') {
            char sign = _expr[_pos++];
            if (!parseFactor(out, error)) {
                return false;
            }
            if (sign == '-') {
                *out = -*out;
            }
            return true;
        }

        if (std::isdigit(static_cast<unsigned char>(_expr[_pos])) || _expr[_pos] == '.') {
            return parseNumber(out, error);
        }

        std::string ident;
        if (!parseIdent(&ident)) {
            return setError(error, "expected number/identifier/factor");
        }
        auto value = _var_resolver(ident);
        if (!value.has_value()) {
            return setError(error, "unknown identifier: " + ident);
        }
        *out = *value;
        return true;
    }

    bool parseNumber(double* out, std::string* error) {
        const char* begin = _expr.c_str() + _pos;
        char* end = nullptr;
        errno = 0;
        double value = std::strtod(begin, &end);
        if (begin == end) {
            return setError(error, "invalid numeric token");
        }
        if (errno == ERANGE) {
            return setError(error, "numeric token out of range");
        }
        _pos += static_cast<size_t>(end - begin);
        *out = value;
        return true;
    }

    bool parseIdent(std::string* out) {
        size_t start = _pos;
        while (_pos < _expr.size()) {
            char c = _expr[_pos];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
                _pos++;
                continue;
            }
            break;
        }
        if (_pos == start) {
            return false;
        }
        *out = _expr.substr(start, _pos - start);
        return true;
    }

private:
    const std::string& _expr;
    size_t _pos = 0;
    std::function<std::optional<double>(const std::string&)> _var_resolver;
};

std::optional<double> resolveExpr(const std::string& expr,
                                  const GlobalNetworkParams& global,
                                  const FlowBasicParams& flow) {
    std::unordered_map<std::string, double> vars = {
        {"flow.peer_rtt_ps", static_cast<double>(flow.peer_rtt_ps)},
        {"flow.bdp_bytes", static_cast<double>(flow.bdp_bytes)},
        {"flow.nic_linkspeed_bps", static_cast<double>(flow.nic_linkspeed_bps)},
        {"flow.mtu_bytes", static_cast<double>(flow.mtu_bytes)},
        {"flow.mss_bytes", static_cast<double>(flow.mss_bytes)},
        {"flow.init_cwnd", static_cast<double>(flow.init_cwnd)},
        {"global.network_rtt_ps", static_cast<double>(global.network_rtt_ps)},
        {"global.network_bdp_bytes", static_cast<double>(global.network_bdp_bytes)},
        {"global.network_linkspeed_bps", static_cast<double>(global.network_linkspeed_bps)},
        {"global.trimming_enabled", global.trimming_enabled ? 1.0 : 0.0},
        // Shorthand aliases.
        {"peer_rtt_ps", static_cast<double>(flow.peer_rtt_ps)},
        {"bdp_bytes", static_cast<double>(flow.bdp_bytes)},
        {"nic_linkspeed_bps", static_cast<double>(flow.nic_linkspeed_bps)},
        {"mtu_bytes", static_cast<double>(flow.mtu_bytes)},
        {"mss_bytes", static_cast<double>(flow.mss_bytes)},
        {"init_cwnd", static_cast<double>(flow.init_cwnd)},
        {"network_rtt_ps", static_cast<double>(global.network_rtt_ps)},
        {"network_bdp_bytes", static_cast<double>(global.network_bdp_bytes)},
        {"network_linkspeed_bps", static_cast<double>(global.network_linkspeed_bps)},
    };

    ExprParser parser(expr, [&vars](const std::string& key) -> std::optional<double> {
        auto it = vars.find(key);
        if (it == vars.end()) {
            return std::nullopt;
        }
        return it->second;
    });

    double value = 0.0;
    std::string error;
    if (!parser.parse(&value, &error)) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

bool CcProfileStore::loadFromJsonFile(const std::string& path, std::string* error) {
    std::ifstream ifs(path);
    if (!ifs) {
        if (error) {
            *error = "failed to open cc_profile file: " + path;
        }
        return false;
    }

    std::stringstream buffer;
    buffer << ifs.rdbuf();
    std::string json_text = buffer.str();

    JsonValue root;
    JsonParser parser(json_text);
    if (!parser.parse(&root, error)) {
        return false;
    }
    if (!root.isObject()) {
        if (error) {
            *error = "cc_profile root must be an object";
        }
        return false;
    }

    const JsonValue* cc = root.find("cc");
    if (cc == nullptr) {
        cc = &root;
    }
    if (!cc->isObject()) {
        if (error) {
            *error = "cc section must be an object";
        }
        return false;
    }

    _defaults.clear();
    _profiles.clear();

    const JsonValue* defaults = cc->find("defaults");
    if (defaults != nullptr) {
        if (!defaults->isObject()) {
            if (error) {
                *error = "cc.defaults must be an object";
            }
            return false;
        }
        for (const auto& kv : defaults->object_value) {
            if (!kv.second || !kv.second->isString()) {
                if (error) {
                    *error = "cc.defaults values must be strings";
                }
                return false;
            }
            CcAlgoId default_algo;
            if (!CcProfileFlowSelectionResolver::parseAlgo(kv.first, &default_algo)) {
                if (error) {
                    *error = "unsupported algo key in cc.defaults: '" + kv.first + "'";
                }
                return false;
            }
            _defaults[default_algo] = kv.second->string_value;
        }
    }

    const JsonValue* profiles = cc->find("profiles");
    if (profiles == nullptr || !profiles->isObject()) {
        if (error) {
            *error = "cc.profiles must be an object";
        }
        return false;
    }

    for (const auto& kv : profiles->object_value) {
        const std::string& profile_id = kv.first;
        if (!kv.second) {
            if (error) {
                *error = "profile entry cannot be null";
            }
            return false;
        }
        const JsonValue& profile_obj = *kv.second;
        if (!profile_obj.isObject()) {
            if (error) {
                *error = "each profile entry must be an object";
            }
            return false;
        }

        const JsonValue* algo = profile_obj.find("algo");
        if (algo == nullptr || !algo->isString()) {
            if (error) {
                *error = "profile '" + profile_id + "' must contain string field 'algo'";
            }
            return false;
        }

        CcProfile profile;
        CcAlgoId profile_algo;
        if (!CcProfileFlowSelectionResolver::parseAlgo(algo->string_value, &profile_algo)) {
            if (error) {
                *error = "profile '" + profile_id + "' uses unsupported algo '" + algo->string_value + "'";
            }
            return false;
        }
        profile.id = profile_id;
        profile.algo = profile_algo;

        const JsonValue* params = profile_obj.find("params");
        if (params != nullptr) {
            if (!params->isObject()) {
                if (error) {
                    *error = "profile '" + profile_id + "'.params must be an object";
                }
                return false;
            }

            for (const auto& pv : params->object_value) {
                if (!pv.second) {
                    if (error) {
                        *error = "profile '" + profile_id + "'.params." + pv.first + " cannot be null";
                    }
                    return false;
                }
                CcProfileParam param;
                if (pv.second->isNumber()) {
                    param.kind = CcProfileParam::Kind::NUMBER;
                    param.number = pv.second->number_value;
                } else if (pv.second->isBool()) {
                    param.kind = CcProfileParam::Kind::BOOL;
                    param.boolean = pv.second->bool_value;
                } else if (pv.second->isString()) {
                    param.kind = CcProfileParam::Kind::EXPR;
                    param.expr = pv.second->string_value;
                } else if (pv.second->isObject()) {
                    const JsonValue* expr = pv.second->find("expr");
                    if (expr == nullptr || !expr->isString()) {
                        if (error) {
                            *error = "profile '" + profile_id + "'.params." + pv.first +
                                     " object must contain string field 'expr'";
                        }
                        return false;
                    }
                    param.kind = CcProfileParam::Kind::EXPR;
                    param.expr = expr->string_value;
                } else {
                    if (error) {
                        *error = "unsupported parameter value type for profile '" + profile_id +
                                 "', key '" + pv.first + "'";
                    }
                    return false;
                }
                profile.params[pv.first] = param;
            }
        }

        _profiles[profile_id] = profile;
    }

    return true;
}

std::optional<CcProfile> CcProfileStore::findById(const std::string& profile_id) const {
    auto it = _profiles.find(profile_id);
    if (it == _profiles.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<CcProfile> CcProfileStore::findDefaultForAlgo(CcAlgoId algo) const {
    auto dit = _defaults.find(algo);
    if (dit == _defaults.end()) {
        return std::nullopt;
    }
    auto pit = _profiles.find(dit->second);
    if (pit == _profiles.end()) {
        return std::nullopt;
    }
    return pit->second;
}

const char* CcProfileFlowSelectionResolver::algoName(CcAlgoId algo) {
    switch (algo) {
        case CcAlgoId::NSCC:
            return "nscc";
        case CcAlgoId::DCTCP:
            return "dctcp";
        case CcAlgoId::CONSTANT:
            return "constant";
        case CcAlgoId::SWIFT:
            return "swift";
        case CcAlgoId::BARRE:
            return "barre";
    }
    return "";
}

bool CcProfileFlowSelectionResolver::parseAlgo(const std::string& algo, CcAlgoId* out) {
    if (out == nullptr) {
        return false;
    }
    if (algo == "nscc") {
        *out = CcAlgoId::NSCC;
        return true;
    }
    if (algo == "dctcp") {
        *out = CcAlgoId::DCTCP;
        return true;
    }
    if (algo == "constant") {
        *out = CcAlgoId::CONSTANT;
        return true;
    }
    if (algo == "swift") {
        *out = CcAlgoId::SWIFT;
        return true;
    }
    if (algo == "barre") {
        *out = CcAlgoId::BARRE;
        return true;
    }
    return false;
}

bool CcProfileFlowSelectionResolver::resolve(const FlowCcOverrideInput& input,
                                             CcAlgoId global_algo,
                                             const CcProfileStore* store,
                                             FlowCcSelectionSpec* out,
                                             std::string* error) {
    if (out == nullptr) {
        if (error) {
            *error = "output pointer is null";
        }
        return false;
    }

    std::optional<CcProfile> flow_profile;
    const std::optional<CcAlgoId> flow_algo = input.algo;

    if (input.profile_id.has_value()) {
        if (store == nullptr) {
            if (error) {
                *error = "flow sets cc_profile but global -cc_profile file is not loaded";
            }
            return false;
        }

        auto profile = store->findById(*input.profile_id);
        if (!profile.has_value()) {
            if (error) {
                *error = "cc_profile id '" + *input.profile_id + "' not found in loaded profile file";
            }
            return false;
        }
        flow_profile = *profile;
    }

    out->profile = flow_profile;
    if (flow_profile.has_value()) {
        CcAlgoId profile_algo = flow_profile->algo;

        if (flow_algo.has_value() && *flow_algo != profile_algo) {
            if (error) {
                *error = "cc_algo '" + std::string(algoName(*flow_algo))
                         + "' mismatches profile algo '" + std::string(algoName(profile_algo)) + "'";
            }
            return false;
        }

        out->algo = flow_algo.has_value() ? *flow_algo : profile_algo;
        return true;
    }

    out->algo = flow_algo.has_value() ? *flow_algo : global_algo;
    if (store != nullptr) {
        auto default_profile = store->findDefaultForAlgo(out->algo);
        if (default_profile.has_value()) {
            out->profile = *default_profile;
        }
    }
    return true;
}

std::optional<double> CcProfileResolver::resolve(const CcProfile& profile,
                                                 const std::string& key,
                                                 const GlobalNetworkParams& global,
                                                 const FlowBasicParams& flow) {
    auto it = profile.params.find(key);
    if (it == profile.params.end()) {
        return std::nullopt;
    }

    const CcProfileParam& param = it->second;
    if (param.kind == CcProfileParam::Kind::NUMBER) {
        return param.number;
    }
    if (param.kind == CcProfileParam::Kind::BOOL) {
        return param.boolean ? 1.0 : 0.0;
    }

    return resolveExpr(param.expr, global, flow);
}

double CcProfileResolver::getDouble(const CcProfile& profile,
                                    const std::string& key,
                                    double default_value,
                                    const GlobalNetworkParams& global,
                                    const FlowBasicParams& flow) {
    auto v = resolve(profile, key, global, flow);
    if (!v.has_value()) {
        return default_value;
    }
    return *v;
}

mem_b CcProfileResolver::getMemB(const CcProfile& profile,
                                 const std::string& key,
                                 mem_b default_value,
                                 const GlobalNetworkParams& global,
                                 const FlowBasicParams& flow) {
    auto v = resolve(profile, key, global, flow);
    if (!v.has_value()) {
        return default_value;
    }
    if (*v < 0.0) {
        return 0;
    }
    if (*v >= static_cast<double>(std::numeric_limits<mem_b>::max())) {
        return std::numeric_limits<mem_b>::max();
    }
    return static_cast<mem_b>(*v);
}

simtime_picosec CcProfileResolver::getTimePs(const CcProfile& profile,
                                             const std::string& key,
                                             simtime_picosec default_value,
                                             const GlobalNetworkParams& global,
                                             const FlowBasicParams& flow) {
    auto v = resolve(profile, key, global, flow);
    if (!v.has_value()) {
        return default_value;
    }
    if (*v < 0.0) {
        return 0;
    }
    if (*v >= static_cast<double>(std::numeric_limits<simtime_picosec>::max())) {
        return std::numeric_limits<simtime_picosec>::max();
    }
    return static_cast<simtime_picosec>(*v);
}

uint32_t CcProfileResolver::getUint32(const CcProfile& profile,
                                      const std::string& key,
                                      uint32_t default_value,
                                      const GlobalNetworkParams& global,
                                      const FlowBasicParams& flow) {
    auto v = resolve(profile, key, global, flow);
    if (!v.has_value()) {
        return default_value;
    }
    if (*v < 0.0) {
        return 0;
    }
    if (*v >= static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(*v);
}

uint8_t CcProfileResolver::getUint8(const CcProfile& profile,
                                    const std::string& key,
                                    uint8_t default_value,
                                    const GlobalNetworkParams& global,
                                    const FlowBasicParams& flow) {
    auto v = resolve(profile, key, global, flow);
    if (!v.has_value()) {
        return default_value;
    }
    if (*v < 0.0) {
        return 0;
    }
    if (*v >= static_cast<double>(std::numeric_limits<uint8_t>::max())) {
        return std::numeric_limits<uint8_t>::max();
    }
    return static_cast<uint8_t>(*v);
}

bool CcProfileResolver::getBool(const CcProfile& profile,
                                const std::string& key,
                                bool default_value,
                                const GlobalNetworkParams& global,
                                const FlowBasicParams& flow) {
    auto it = profile.params.find(key);
    if (it == profile.params.end()) {
        return default_value;
    }

    if (it->second.kind == CcProfileParam::Kind::BOOL) {
        return it->second.boolean;
    }

    auto v = resolve(profile, key, global, flow);
    if (!v.has_value()) {
        return default_value;
    }
    return std::fabs(*v) > 1e-12;
}
