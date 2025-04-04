/***************************************************************************
* Copyright (c) 2023, QuantStack
*
* Distributed under the terms of the GNU General Public License v3.
*
* The full license is in the file LICENSE, distributed with this software.
****************************************************************************/

#include <string>
#include <vector>
#include <iostream>

#include "nlohmann/json.hpp"

#include "xeus/xinput.hpp"
#include "xeus/xinterpreter.hpp"
#include "xeus/xhelper.hpp"

#include "xeus-r/xinterpreter.hpp"

#define R_NO_REMAP
#define R_INTERFACE_PTRS

#include "R.h"
#include "Rinternals.h"
#include "Rembedded.h"
#include "R_ext/Parse.h"
#include "R_ext/Rdynload.h"
#include "Rversion.h"

#ifndef _WIN32
#include "Rinterface.h"
#endif

#include "rtools.hpp"
#include <algorithm>
#include <cstddef>

namespace xeus_r {

static interpreter* p_interpreter = nullptr;

interpreter* get_interpreter() {
    return p_interpreter;
}

void WriteConsoleEx(const char *buf, int buflen, int otype) {
    std::string output(buf, buflen);
    if (otype == 1) {
        p_interpreter->publish_stream("stderr", output);
    } else {
        p_interpreter->publish_stream("stdout", output);
    }
}

void capture_WriteConsoleEx(const char *buf, int buflen, int otype) {
    std::string output(buf, buflen);
    if (otype == 1) {
        // do nothing
    } else {
        p_interpreter->capture_stream << output;
    }
}

int ReadConsole(const char *prompt, unsigned char *buffer, int length, int /*addtohistory*/) {
    std::string res = xeus::blocking_input_request(prompt, false);
    
    std::size_t size = std::min(res.size(), std::size_t(length));
    std::copy(res.c_str(), res.c_str() + size , buffer);
    buffer[size] = '\n';

    // TODO: return 0 when xeus::blocking_input_request fails
    return 1;
}

interpreter::interpreter(int argc, char* argv[])
{
    // When building with Emscripten, pass --no-readline to disable
    // readline support, as r-base is not compiled with readline
    // and will not read input from the command line.
#ifdef __EMSCRIPTEN__
    const char* argvNew[] = {"--no-readline", "--vanilla"};
    Rf_initEmbeddedR(sizeof(argvNew) / sizeof(argvNew[0]), const_cast<char**>(argvNew));
#else
    Rf_initEmbeddedR(argc, argv);
#endif

    register_r_routines();

#ifndef _WIN32
    R_Outputfile = NULL;
    R_Consolefile = NULL;

    ptr_R_WriteConsole = nullptr;
    ptr_R_WriteConsoleEx = WriteConsoleEx;
    ptr_R_ReadConsole = ReadConsole;
#endif

    xeus::register_interpreter(this);
    p_interpreter = this;
}


void interpreter::execute_request_impl(
    send_reply_callback cb,
    int execution_count,
    const std::string& code,
    xeus::execute_request_config config,
    nl::json /*user_expressions*/
)
{
    if (config.store_history) {
        const_cast<xeus::xhistory_manager&>(get_history_manager()).store_inputs(0, execution_count, code);
    }

    SEXP code_ = PROTECT(Rf_mkString(code.c_str()));
    SEXP execution_counter_ = PROTECT(Rf_ScalarInteger(execution_count));
    SEXP silent_ = PROTECT(Rf_ScalarLogical(config.silent));

    SEXP result = r::invoke_hera_fn("execute", code_, execution_counter_, silent_);

    if (Rf_inherits(result, "error_reply")) {
        std::string evalue = CHAR(STRING_ELT(VECTOR_ELT(result, 0), 0));
        std::string ename = CHAR(STRING_ELT(VECTOR_ELT(result, 1), 0));

        std::vector<std::string> trace_back;
        if (XLENGTH(result) > 2) {
            SEXP trace_back_ = VECTOR_ELT(result, 2);
            auto n = XLENGTH(trace_back_);
            for (decltype(n) i = 0; i < n; i++) {
                trace_back.push_back(CHAR(STRING_ELT(trace_back_, i)));
            }
        }

        publish_execution_error(evalue, ename, trace_back);

        UNPROTECT(3);
        cb(xeus::create_error_reply(evalue, ename, std::move(trace_back)));
    }

    if (Rf_inherits(result, "execution_result")) {
        SEXP data_ = VECTOR_ELT(result, 0);
        SEXP metadata_ = VECTOR_ELT(result, 1);
        auto data = nl::json::parse(CHAR(STRING_ELT(data_, 0)));
        auto metadata = nl::json::parse(CHAR(STRING_ELT(metadata_, 0)));
        publish_execution_result(execution_count, data, metadata);
    }

    UNPROTECT(3);
    cb(xeus::create_successful_reply(/*payload, user_expressions*/));
}

void interpreter::configure_impl()
{
    SEXP sym_library       = Rf_install("require");
    SEXP str_hera          = PROTECT(Rf_mkString("hera"));
    SEXP sym_quietly       = Rf_install("quietly");
    SEXP call_library_hera = PROTECT(xeus_r::r::r_call(sym_library, str_hera, /* quietly = */ Rf_ScalarLogical(TRUE)));
    SET_TAG(CDDR(call_library_hera), sym_quietly);
    SEXP out = PROTECT(Rf_eval(call_library_hera, R_GlobalEnv));
    if (LOGICAL_ELT(out, 0) == FALSE) {
        // TODO: suicide the kernel because hera is not installed
    }

    UNPROTECT(3);
}

nl::json interpreter::is_complete_request_impl(const std::string& code_)
{
    // initially code holds the string, but then it is being
    // replaced by incomplete, invalid or complete either in the
    // body handler or the error handler
    SEXP code = PROTECT(Rf_mkString(code_.c_str()));

    // we can't simply use an R callback because the R parse(text =)
    // approach does not distinguish between invalid code and
    // incomplete: they both just throw an error
    R_tryCatchError(
        [](void* void_code) { // body
            ParseStatus status;
            SEXP code = reinterpret_cast<SEXP>(void_code);

            R_ParseVector(code, -1, &status, R_NilValue);

            switch(status) {
                case PARSE_INCOMPLETE:
                    SET_STRING_ELT(code, 0, Rf_mkChar("incomplete"));
                    break;

                case PARSE_ERROR:
                    SET_STRING_ELT(code, 0, Rf_mkChar("invalid"));
                    break;

                default:
                    SET_STRING_ELT(code, 0, Rf_mkChar("complete"));
            }

            return R_NilValue;
        },
        reinterpret_cast<void*>(code),

        [](SEXP, void* void_code) { // handler
            // some parse error cases are not propagated to PARSE_ERROR
            // but rather throw an error, so we need to catch it
            // and set the result to invalid
            SEXP code = reinterpret_cast<SEXP>(void_code);
            SET_STRING_ELT(code, 0, Rf_mkChar("invalid"));

            return R_NilValue;
        },
        reinterpret_cast<void*>(code)
    );

    // eventually we just have to extract the string from code (which has been replaced)
    auto result = xeus::create_is_complete_reply(CHAR(STRING_ELT(code, 0)), "");
    UNPROTECT(1);
    return result;
}

nl::json json_from_character_vector(SEXP x) {
    auto n = XLENGTH(x);
    std::vector<std::string> vec(n);

    for (decltype(n) i = 0; i < n; i++) {
        vec[i] = std::string(CHAR(STRING_ELT(x, i)));
    }
    return nl::json(vec);
}

nl::json interpreter::complete_request_impl(const std::string& code, int cursor_pos)
{
    SEXP code_ = PROTECT(Rf_mkString(code.c_str()));
    SEXP cursor_pos_ = PROTECT(Rf_ScalarInteger(cursor_pos));

    SEXP result = PROTECT(r::invoke_hera_fn("complete", code_, cursor_pos_));

    auto matches = json_from_character_vector(VECTOR_ELT(result, 0));
    int cursor_start = INTEGER_ELT(VECTOR_ELT(result, 1), 0);
    int cursor_end = INTEGER_ELT(VECTOR_ELT(result, 1), 1);

    auto reply = xeus::create_complete_reply(
        matches, cursor_start, cursor_end
    );

    UNPROTECT(3); // result, cursor_pos_, code_
    return reply;
}

nl::json interpreter::inspect_request_impl(const std::string& code, int cursor_pos, int /*detail_level*/)
{
    SEXP code_ = PROTECT(Rf_mkString(code.c_str()));
    SEXP cursor_pos_ = PROTECT(Rf_ScalarInteger(cursor_pos));

    SEXP result = PROTECT(r::invoke_hera_fn("inspect", code_, cursor_pos_));
    bool found = LOGICAL_ELT(VECTOR_ELT(result, 0), 0);
    if (!found) {
        UNPROTECT(3);
        return xeus::create_inspect_reply(false);
    }

    auto data = nl::json::parse(CHAR(STRING_ELT(VECTOR_ELT(result, 1), 0)));

    UNPROTECT(3); // result, code_, cursor_pos_
    return xeus::create_inspect_reply(found, data);
}

void interpreter::shutdown_request_impl() {
    Rf_endEmbeddedR(0);
}

nl::json interpreter::kernel_info_request_impl()
{
    const std::string  protocol_version = "5.3";
    const std::string  implementation = "xr";
    const std::string  implementation_version = XEUS_R_VERSION;
    const std::string  language_name = "R";
    const std::string  language_version = std::string(R_MAJOR) + "." + std::string(R_MINOR);
    const std::string  language_mimetype = "text/x-R";
    const std::string  language_file_extension = "R";
    const std::string  language_pygments_lexer = "";
    const std::string  language_codemirror_mode = "";
    const std::string  language_nbconvert_exporter = "";
    const std::string  banner = "xr";
    const bool         debugger = false;
    const nl::json     help_links = nl::json::array();

    return xeus::create_info_reply(
        protocol_version,
        implementation,
        implementation_version,
        language_name,
        language_version,
        language_mimetype,
        language_file_extension,
        language_pygments_lexer,
        language_codemirror_mode,
        language_nbconvert_exporter,
        banner,
        debugger,
        help_links
    );
}

}
