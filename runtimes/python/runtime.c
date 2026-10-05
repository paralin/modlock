// The Python runtime runs Python mods. It is a WASI reactor with the same
// boundary as a compiled mod: the host hands the start event, which carries
// the mod's sources as an uncompressed zip in StartEvent.source, and the
// runtime imports the mod's main module. Every event, the start event
// included, then goes to the function the library registers with
// _modlock.handle, which returns the encoded Reply.
//
// modlock_initialize runs once, when the release builds python.wasm: Wizer
// snapshots the module after it, with the interpreter, the prelude and the
// standard modules it imports in memory, so an instance starts without
// initializing Python or reading a file.

#include <Python.h>

#include "../boundary.h"

// prelude imports the standard modules and defines start and describe.
static const char prelude[] = {
#embed "prelude.py"
    , 0};

// start is the prelude's start(sources), describe its describe(error), and
// handler the event function the library registered.
static PyObject* start;
static PyObject* describe;
static PyObject* handler;

// result holds the last encoded Reply until the next event.
static PyObject* result;

// LogError logs the raised exception under what, with its traceback.
static void LogError(const char* what) {
  PyObject* error = PyErr_GetRaisedException();
  PyObject* text = describe ? PyObject_CallOneArg(describe, error) : NULL;
  Py_ssize_t size = 0;
  const char* line = text ? PyUnicode_AsUTF8AndSize(text, &size) : NULL;
  if (!line) {
    PyErr_Clear();
    line = "an error the runtime cannot describe";
    size = (Py_ssize_t)strlen(line);
  }
  size_t what_size = strlen(what);
  char* message = malloc(what_size + 2 + (size_t)size);
  if (!message) __builtin_trap();
  memcpy(message, what, what_size);
  memcpy(message + what_size, ": ", 2);
  memcpy(message + what_size + 2, line, (size_t)size);
  host_log(message, what_size + 2 + (size_t)size);
  free(message);
  Py_XDECREF(text);
  Py_DECREF(error);
}

// HostCall is _modlock.call(call: bytes) -> bytes, which exchanges an encoded
// Call for the host's encoded Reply.
static PyObject* HostCall(PyObject* self, PyObject* call) {
  char* request;
  Py_ssize_t size;
  if (PyBytes_AsStringAndSize(call, &request, &size) < 0) return NULL;
  uint32_t response_size;
  uint8_t* response = exchange((const uint8_t*)request, (size_t)size, &response_size);
  PyObject* reply = PyBytes_FromStringAndSize((const char*)response, response_size);
  free(response);
  return reply;
}

// HostLog is _modlock.log(text: str), which writes one line to the server log.
static PyObject* HostLog(PyObject* self, PyObject* text) {
  Py_ssize_t size;
  const char* line = PyUnicode_AsUTF8AndSize(text, &size);
  if (!line) return NULL;
  host_log(line, (size_t)size);
  Py_RETURN_NONE;
}

// Handle is _modlock.handle(event: Callable[[bytes], bytes]), which registers
// the function every event goes to.
static PyObject* Handle(PyObject* self, PyObject* event) {
  if (!PyCallable_Check(event)) {
    PyErr_SetString(PyExc_TypeError, "the event handler must be callable");
    return NULL;
  }
  Py_XSETREF(handler, Py_NewRef(event));
  Py_RETURN_NONE;
}

static PyMethodDef bridge_methods[] = {
    {"call", HostCall, METH_O, "Exchange an encoded Call for the encoded Reply."},
    {"log", HostLog, METH_O, "Write one line to the server log."},
    {"handle", Handle, METH_O, "Register the function every event goes to."},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef bridge = {
    PyModuleDef_HEAD_INIT, "_modlock", "The bridge between the modlock library and the host.", -1,
    bridge_methods,
};

static PyObject* InitBridge(void) { return PyModule_Create(&bridge); }

// Keep adds one file of the mod's zip to the sources dict.
static int Keep(void* sources, const char* name, size_t name_size, const uint8_t* data,
                size_t size) {
  PyObject* key = PyUnicode_DecodeUTF8(name, (Py_ssize_t)name_size, NULL);
  PyObject* value = PyBytes_FromStringAndSize((const char*)data, (Py_ssize_t)size);
  int kept = key && value && PyDict_SetItem(sources, key, value) == 0;
  Py_XDECREF(key);
  Py_XDECREF(value);
  return kept;
}

// Start reads the mod's sources from the start event and imports its main
// module.
static int Start(const uint8_t* event, size_t size) {
  const uint8_t* zip;
  size_t zip_size;
  if (!start_source(event, size, &zip, &zip_size)) {
    static const char missing[] = "the start event carries no Python sources";
    host_log(missing, sizeof(missing) - 1);
    return 0;
  }
  PyObject* sources = PyDict_New();
  if (!sources || !read_zip(zip, zip_size, Keep, sources)) {
    PyErr_Clear();
    Py_XDECREF(sources);
    static const char unreadable[] = "the mod's sources are not a readable uncompressed zip";
    host_log(unreadable, sizeof(unreadable) - 1);
    return 0;
  }
  PyObject* started = PyObject_CallOneArg(start, sources);
  Py_DECREF(sources);
  if (!started) {
    LogError("start");
    return 0;
  }
  Py_DECREF(started);
  Py_CLEAR(start);
  return 1;
}

// modlock_initialize starts the interpreter with the standard library at
// /lib and runs the prelude. Only the release build calls it, under Wizer.
__attribute__((export_name("modlock_initialize"))) void modlock_initialize(void) {
  if (PyImport_AppendInittab("_modlock", InitBridge) < 0) __builtin_trap();
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.site_import = 0;
  config.write_bytecode = 0;
  config.use_hash_seed = 1;
  config.hash_seed = 0;
  PyStatus status = PyConfig_SetString(&config, &config.home, L"/");
  if (!PyStatus_Exception(status)) status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status)) Py_ExitStatusException(status);

  PyObject* globals = PyDict_New();
  if (!globals || PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) < 0) {
    __builtin_trap();
  }
  PyObject* ran = PyRun_String(prelude, Py_file_input, globals, globals);
  if (!ran) {
    PyErr_Print();
    __builtin_trap();
  }
  Py_DECREF(ran);
  start = Py_XNewRef(PyDict_GetItemString(globals, "start"));
  describe = Py_XNewRef(PyDict_GetItemString(globals, "describe"));
  Py_DECREF(globals);
  if (!start || !describe) __builtin_trap();
}

// modlock_event receives one encoded Call of size bytes, hands it to the
// library's handler, and returns the encoded Reply's address in the high 32
// bits and its length in the low 32 bits. A mod that fails to start traps,
// which stops it.
__attribute__((export_name("modlock_event"))) uint64_t modlock_event(uint32_t size) {
  // Copy the event out of the host.
  PyObject* event = PyBytes_FromStringAndSize(NULL, size);
  if (!event) __builtin_trap();
  host_read(PyBytes_AS_STRING(event), size);
  const int starting = start != NULL;
  if (starting && !Start((const uint8_t*)PyBytes_AS_STRING(event), size)) __builtin_trap();
  if (!handler) {
    Py_DECREF(event);
    if (starting) {
      static const char missing[] = "the mod's library registered no event handler";
      host_log(missing, sizeof(missing) - 1);
      __builtin_trap();
    }
    return 0;
  }

  // Hand it to the handler and keep its answer until the next event.
  Py_CLEAR(result);
  result = PyObject_CallOneArg(handler, event);
  Py_DECREF(event);
  if (!result) {
    LogError("event");
    if (starting) __builtin_trap();
    return 0;
  }
  if (!PyBytes_Check(result) || PyBytes_GET_SIZE(result) == 0) return 0;
  return (uint64_t)(uintptr_t)PyBytes_AS_STRING(result) << 32 | (uint64_t)PyBytes_GET_SIZE(result);
}
