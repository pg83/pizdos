/* The python of pizdos: CPython's main, whose standard library is the
 * program's own file. The library's zip is the tail of python's executable
 * (dev/exe.py), past what the kernel reads, where zipimport finds it from
 * the end of the file, and the kernel tells a program its file's path:
 * dladdr's. sys.path is then
 * PYTHONPATH's directories, when the environment counts, this file and
 * STDLIB_DIR, the standard library's directory of a root that has one,
 * with no search for a prefix and none of its warnings: nothing of the
 * library is outside the file. A program the kernel knows no path of
 * searches as CPython does. */

#include "Python.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

/* append adds a path to a list of paths: one of the environment's made
 * absolute from the working directory, as CPython makes it. */
static PyStatus append(PyWideStringList* list, const wchar_t* path, size_t n) {
    wchar_t* cwd = NULL;

    if (n == 0) {
        return PyStatus_Ok();
    }

    if (path[0] != L'/') {
        char here[4096];

        if (getcwd(here, sizeof here) != NULL) {
            cwd = Py_DecodeLocale(here, NULL);
        }
    }

    size_t head = cwd != NULL ? wcslen(cwd) + 1 : 0;
    wchar_t* full = PyMem_RawMalloc((head + n + 1) * sizeof(wchar_t));

    if (full == NULL) {
        PyMem_RawFree(cwd);

        return PyStatus_NoMemory();
    }

    if (cwd != NULL) {
        wcscpy(full, cwd);
        full[head - 1] = L'/';
        PyMem_RawFree(cwd);
    }

    wmemcpy(full + head, path, n);
    full[head + n] = 0;
    PyStatus status = PyWideStringList_Append(list, full);
    PyMem_RawFree(full);

    return status;
}

static PyStatus configure(PyConfig* config, int argc, char** argv) {
    PyStatus status = PyConfig_SetBytesArgv(config, argc, argv);

    if (PyStatus_Exception(status)) {
        return status;
    }

    status = PyConfig_Read(config);

    if (PyStatus_Exception(status)) {
        return status;
    }

    Dl_info self;

    if (!dladdr((void*)configure, &self) || self.dli_fname == NULL || self.dli_fname[0] == 0) {
        return PyStatus_Ok();
    }

    config->pathconfig_warnings = 0;
    config->module_search_paths_set = 1;

    if (config->use_environment && config->pythonpath_env != NULL) {
        const wchar_t* at = config->pythonpath_env;

        for (;;) {
            const wchar_t* end = wcschr(at, L':');
            size_t n = end != NULL ? (size_t)(end - at) : wcslen(at);
            status = append(&config->module_search_paths, at, n);

            if (PyStatus_Exception(status) || end == NULL) {
                break;
            }

            at = end + 1;
        }

        if (PyStatus_Exception(status)) {
            return status;
        }
    }

    wchar_t* file = Py_DecodeLocale(self.dli_fname, NULL);

    if (file == NULL) {
        return PyStatus_NoMemory();
    }

    status = append(&config->module_search_paths, file, wcslen(file));
    PyMem_RawFree(file);

    if (PyStatus_Exception(status)) {
        return status;
    }

    return append(&config->module_search_paths, STDLIB_DIR, wcslen(STDLIB_DIR));
}

int main(int argc, char** argv) {
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    PyStatus status = configure(&config, argc, argv);

    if (!PyStatus_Exception(status)) {
        status = Py_InitializeFromConfig(&config);
    }

    PyConfig_Clear(&config);

    if (PyStatus_Exception(status)) {
        if (PyStatus_IsExit(status)) {
            return status.exitcode;
        }

        Py_ExitStatusException(status);
    }

    return Py_RunMain();
}
