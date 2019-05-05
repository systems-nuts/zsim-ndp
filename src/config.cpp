/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include <sstream>
#include <string.h>
#include <string>
#include <typeinfo>
#include <vector>
#include "libconfig.h"
#include "log.h"

// We need minor specializations to work with older versions of libconfig
#if defined(LIBCONFIGXX_VER_MAJOR) && defined(LIBCONFIGXX_VER_MINOR) && defined(LIBCONFIGXX_VER_REVISION)
#define LIBCONFIG_VERSION (LIBCONFIGXX_VER_MAJOR*10000 +  LIBCONFIGXX_VER_MINOR*100 + LIBCONFIGXX_VER_REVISION)
#else
#define LIBCONFIG_VERSION 0
#endif

using std::string;
using std::stringstream;
using std::vector;

// Restrict use of long long, which libconfig uses as its int64
typedef long long lc_int64;  // NOLINT(runtime/int)

Config::Config(const char* inFile) {
    inCfg = new config_t;
    outCfg = new config_t;
    config_init(inCfg);
    config_init(outCfg);
    config_read_file(inCfg, inFile);
    auto err = config_error_type(inCfg);
    if (err == CONFIG_ERR_FILE_IO) {
        panic("Input config file %s could not be read", inFile);
    } else if (err == CONFIG_ERR_PARSE) {
#if LIBCONFIG_VERSION >= 10408 // 1.4.8
        const char* peFile = config_error_file(inCfg);
#else
        // Old versions of libconfig don't have libconfig::ParseException::getFile()
        // Using inFile is typically OK, but won't be accurate with multi-file configs (includes)
        const char* peFile = inFile;
#endif
        panic("Input config file %s could not be parsed, line %d, error: %s", peFile, config_error_line(inCfg), config_error_text(inCfg));
    } else {
        assert(err == CONFIG_ERR_NONE);
    }
}

Config::~Config() {
    config_destroy(inCfg);
    config_destroy(outCfg);
    delete inCfg;
    delete outCfg;
}

// Helper function: Add "*"-prefixed vars, which are used by our scripts but not zsim, to outCfg
// Returns number of copied vars
static uint32_t copyNonSimVars(config_setting_t* s1, config_setting_t* s2, std::string prefix) {
    uint32_t copied = 0;
    for (uint32_t i = 0; i < (uint32_t)config_setting_length(s1); i++) {
        config_setting_t* s1i = config_setting_get_elem(s1, i);
        const char* name = config_setting_name(s1i);
        config_setting_t* s2i = config_setting_get_member(s2, name);
        if (name[0] == '*') {
            if (s2i) panic("Setting %s was read, should be private", (prefix + name).c_str());
            // This could be as simple as:
            // config_seeting_set(config_setting_add(s2, name, config_setting_type(s1i)), config_setting_get(s1i));
            // However, because Setting kinda sucks, we need to go type by type:
            int s1iType = config_setting_type(s1i);
            s2i = config_setting_add(s2, name, s1iType);
            if      (CONFIG_TYPE_INT     == s1iType) config_setting_set_int    (s2i, config_setting_get_int    (s1i));
            else if (CONFIG_TYPE_INT64   == s1iType) config_setting_set_int64  (s2i, config_setting_get_int64  (s1i));
            else if (CONFIG_TYPE_BOOL    == s1iType) config_setting_set_bool   (s2i, config_setting_get_bool   (s1i));
            else if (CONFIG_TYPE_STRING  == s1iType) config_setting_set_string (s2i, config_setting_get_string (s1i));
            else panic("Unknown type for priv setting %s, cannot copy", (prefix + name).c_str());
            copied++;
        }

        if (config_setting_is_group(s1i) && s2i) {
            copied += copyNonSimVars(s1i, s2i, prefix + name + ".");
        }
    }
    return copied;
}

// Helper function: Compares two settings recursively, checking for inclusion
// Returns number of settings without inclusion (given but unused)
static uint32_t checkIncluded(config_setting_t* s1, config_setting_t* s2, std::string prefix) {
    uint32_t unused = 0;
    for (uint32_t i = 0; i < (uint32_t)config_setting_length(s1); i++) {
        config_setting_t* s1i = config_setting_get_elem(s1, i);
        const char* name = config_setting_name(s1i);
        config_setting_t* s2i = config_setting_get_member(s2, name);
        if (!s2i) {
            warn("Setting %s not used during configuration", (prefix + name).c_str());
            unused++;
        } else if (config_setting_is_group(s1i)) {
            unused += checkIncluded(s1i, s2i, prefix + name + ".");
        }
    }
    return unused;
}



//Called when initialization ends. Writes output config, and emits warnings for unused input settings
void Config::writeAndClose(const char* outFile, bool strictCheck) {
    uint32_t nonSimVars = copyNonSimVars(config_root_setting(inCfg), config_root_setting(outCfg), std::string(""));
    uint32_t unused = checkIncluded(config_root_setting(inCfg), config_root_setting(outCfg), std::string(""));

    if (nonSimVars) info("Copied %d non-sim var%s to output config", nonSimVars, (nonSimVars > 1)? "s" : "");
    if (unused) {
        if (strictCheck) {
            panic("%d setting%s not used during configuration", unused, (unused > 1)? "s" : "");
        } else {
            warn("%d setting%s not used during configuration", unused, (unused > 1)? "s" : "");
        }
    }

    config_write_file(outCfg, outFile);
    auto err = config_error_type(outCfg);
    if (err == CONFIG_ERR_FILE_IO) {
        panic("Output config file %s could not be written", outFile);
    }
}


bool Config::exists(const char* key) {
    return config_lookup(inCfg, key) != nullptr;
}

//Helper functions
template<typename T> static const char* getTypeName();
template<> const char* getTypeName<int>() {return "uint32";}
template<> const char* getTypeName<lc_int64>() {return "uint64";}
template<> const char* getTypeName<bool>() {return "bool";}
template<> const char* getTypeName<const char*>() {return "string";}
template<> const char* getTypeName<double>() {return "double";}

typedef int SType;
template<typename T> static SType getSType();
template<> SType getSType<int>() {return CONFIG_TYPE_INT;}
template<> SType getSType<lc_int64>() {return CONFIG_TYPE_INT64;}
template<> SType getSType<bool>() {return CONFIG_TYPE_BOOL;}
template<> SType getSType<const char*>() {return CONFIG_TYPE_STRING;}
template<> SType getSType<double>() {return CONFIG_TYPE_FLOAT;}

template<typename T> static bool getEq(T v1, T v2);
template<> bool getEq<int>(int v1, int v2) {return v1 == v2;}
template<> bool getEq<lc_int64>(lc_int64 v1, lc_int64 v2) {return v1 == v2;}
template<> bool getEq<bool>(bool v1, bool v2) {return v1 == v2;}
template<> bool getEq<const char*>(const char* v1, const char* v2) {return strcmp(v1, v2) == 0;}
template<> bool getEq<double>(double v1, double v2) {return v1 == v2;}

template<typename T> static T getValue(config_setting_t* s);
template<> int getValue<int>(config_setting_t* s) { return config_setting_get_int(s); }
template<> lc_int64 getValue<lc_int64>(config_setting_t* s) { return config_setting_get_int64(s); }
template<> double getValue<double>(config_setting_t* s) { return config_setting_get_float(s); }
template<> bool getValue<bool>(config_setting_t* s) { return config_setting_get_bool(s); }
template<> const char* getValue<const char*>(config_setting_t* s) { return config_setting_get_string(s); }

template<typename T> static int setValue(config_setting_t* s, T v);
template<> int setValue<int>(config_setting_t* s, int v) { return config_setting_set_int(s, v); }
template<> int setValue<lc_int64>(config_setting_t* s, lc_int64 v) { return config_setting_set_int64(s, v); }
template<> int setValue<double>(config_setting_t* s, double v) { return config_setting_set_float(s, v); }
template<> int setValue<bool>(config_setting_t* s, bool v) { return config_setting_set_bool(s, v); }
template<> int setValue<const char*>(config_setting_t* s, const char* v) { return config_setting_set_string(s, v); }

template<typename T> static void writeVar(config_setting_t* setting, const char* key, T val) {
    //info("writeVal %s", key);
    const char* sep = strchr(key, '.');
    if (sep) {
        assert(*sep == '.');
        uint32_t plen = (size_t)(sep-key);
        char prefix[plen+1];
        strncpy(prefix, key, plen);
        prefix[plen] = 0;
        // libconfig strdups all passed strings, so it's fine that prefix is local.
        if (!config_setting_lookup(setting, prefix)) {
            if (!config_setting_add(setting, (const char*)prefix, CONFIG_TYPE_GROUP)) {
                panic("libconfig error adding group setting %s", prefix);
            }
        }
        config_setting_t* child = config_setting_get_member(setting, (const char*)prefix);
        writeVar(child, sep+1, val);
    } else {
        config_setting_t* leaf = config_setting_get_member(setting, key);
        if (!leaf) {
            leaf = config_setting_add(setting, key, getSType<T>());
            if (!leaf) {
                panic("libconfig error adding leaf setting %s", key);
            }
            assert(setValue<T>(leaf, val));
        } else {
            //If this panics, what the hell are you doing in the code? Multiple reads and different defaults??
            T origVal = getValue<T>(leaf);
            if (!getEq(val, origVal)) panic("Duplicate writes to out config key %s with different values!", key);
        }
    }
}

template<typename T> static void writeVar(config_t* cfg, const char* key, T val) {
    writeVar(config_root_setting(cfg), key, val);
}


template<typename T>
T Config::genericGet(const char* key, T def) {
    T val;
    auto setting = config_lookup(inCfg, key);
    if (setting) {
        if (config_setting_type(setting) != getSType<T>()) {
            panic("Type error on optional setting %s, expected type %s", key, getTypeName<T>());
        }
        val = getValue<T>(setting);
    } else {
        val = def;
    }
    writeVar(outCfg, key, val);
    return val;
}

template<typename T>
T Config::genericGet(const char* key) {
    T val;
    auto setting = config_lookup(inCfg, key);
    if (setting) {
        if (config_setting_type(setting) != getSType<T>()) {
            panic("Type error on mandatory setting %s, expected type %s", key, getTypeName<T>());
        }
        val = getValue<T>(setting);
    } else {
        panic("Mandatory setting %s (%s) not found", key, getTypeName<T>())
    }
    writeVar(outCfg, key, val);
    return val;
}

//Template specializations for access interface
template<> uint32_t Config::get<uint32_t>(const char* key) {return (uint32_t) genericGet<int>(key);}
template<> uint64_t Config::get<uint64_t>(const char* key) {return (uint64_t) genericGet<lc_int64>(key);}
template<> bool Config::get<bool>(const char* key) {return genericGet<bool>(key);}
template<> const char* Config::get<const char*>(const char* key) {return genericGet<const char*>(key);}
template<> double Config::get<double>(const char* key) {return (double) genericGet<double>(key);}

template<> uint32_t Config::get<uint32_t>(const char* key, uint32_t def) {return (uint32_t) genericGet<int>(key, (int)def);}
template<> uint64_t Config::get<uint64_t>(const char* key, uint64_t def) {return (uint64_t) genericGet<lc_int64>(key, (lc_int64)def);}
template<> bool Config::get<bool>(const char* key, bool def) {return genericGet<bool>(key, def);}
template<> const char* Config::get<const char*>(const char* key, const char* def) {return genericGet<const char*>(key, def);}
template<> double Config::get<double>(const char* key, double def) {return (double) genericGet<double>(key, (double)def);}

//Get subgroups in a specific key
void Config::subgroups(const char* key, std::vector<const char*>& grps) {
    auto setting = config_lookup(inCfg, key);
    if (setting) {
        uint32_t n = config_setting_length(setting); //0 if not a group or list
        for (uint32_t i = 0; i < n; i++) {
            auto s = config_setting_get_elem(setting, i);
            if (config_setting_is_group(s)) grps.push_back(config_setting_name(s));
        }
    }
}


/* Config value parsing functions */

//Range parsing, for process masks

//Helper, from http://oopweb.com/CPP/Documents/CPPHOWTO/Volume/C++Programming-HOWTO-7.html
void Tokenize(const string& str, vector<string>& tokens, const string& delimiters) {
    // Skip delimiters at beginning.
    string::size_type lastPos = 0; //dsm: DON'T //str.find_first_not_of(delimiters, 0);
    // Find first "non-delimiter".
    string::size_type pos = str.find_first_of(delimiters, lastPos);

    while (string::npos != pos || string::npos != lastPos) {
        // Found a token, add it to the vector.
        tokens.push_back(str.substr(lastPos, pos - lastPos));
        // Skip delimiters.  Note the "not_of"
        lastPos = str.find_first_not_of(delimiters, pos);
        // Find next "non-delimiter"
        pos = str.find_first_of(delimiters, lastPos);
    }
}

struct Range {
    int32_t min;
    int32_t sup;
    int32_t step;

    explicit Range(string r)  {
        vector<string> t;
        Tokenize(r, t, ":");
        vector<uint32_t> n;
        for (auto s : t) {
            stringstream ss(s);
            uint32_t x = 0;
            ss >> x;
            if (ss.fail()) panic("%s in range %s is not a valid number", s.c_str(), r.c_str());
            n.push_back(x);
        }
        switch (n.size()) {
            case 1:
                min = n[0];
                sup = min + 1;
                step = 1;
                break;
            case 2:
                min = n[0];
                sup = n[1];
                step = 1;
                break;
            case 3:
                min = n[0];
                sup = n[1];
                step = n[2];
                break;
            default:
                panic("Range '%s' can only have 1-3 numbers delimited by ':', %ld parsed", r.c_str(), n.size());
        }

        //Final error-checking
        if (min < 0 || step < 0 || sup < 0) panic("Range %s has negative numbers", r.c_str());
        if (step == 0) panic("Range %s has 0 step!", r.c_str());
        if (min >= sup) panic("Range %s has min >= sup!", r.c_str());
    }

    void fill(vector<bool>& mask) {
        for (int32_t i = min; i < sup; i += step) {
            if (i >= (int32_t)mask.size() || i < 0) panic("Range %d:%d:%d includes out-of-bounds %d (mask limit %ld)", min, step, sup, i, mask.size()-1);
            mask[i] = true;
        }
    }
};

std::vector<bool> ParseMask(const std::string& maskStr, uint32_t maskSize) {
    vector<bool> mask;
    mask.resize(maskSize);

    vector<string> ranges;
    Tokenize(maskStr, ranges, " ");
    for (auto r : ranges) {
        if (r.length() == 0) continue;
        Range range(r);
        range.fill(mask);
    }
    return mask;
}

//List parsing
template <typename T>
std::vector<T> ParseList(const std::string& listStr, const char* delimiters) {
    vector<string> nums;
    Tokenize(listStr, nums, delimiters);

    vector<T> res;
    for (auto n : nums) {
        if (n.length() == 0) continue;
        stringstream ss(n);
        T x;
        ss >> x;
        if (ss.fail()) panic("%s in list [%s] could not be parsed", n.c_str(), listStr.c_str());
        res.push_back(x);
    }
    return res;
}

//Instantiations
template std::vector<uint32_t> ParseList<uint32_t>(const std::string& listStr, const char* delimiters);
template std::vector<uint64_t> ParseList<uint64_t>(const std::string& listStr, const char* delimiters);
template std::vector<std::string> ParseList(const std::string& listStr, const char* delimiters);
