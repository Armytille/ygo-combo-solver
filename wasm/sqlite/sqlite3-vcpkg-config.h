/*
 * Version WASM du fichier de configuration que vcpkg injecte dans sqlite3.h.
 * Identique a celui de la construction Windows, SAUF la couche OS : SQLITE_OS_WIN
 * y est remplace par SQLITE_OS_UNIX, la seule que fournit emscripten.
 *
 * Pas de garde d'inclusion : reprend volontairement celle de sqlite3.h.
 */
#define SQLITE_ENABLE_UNLOCK_NOTIFY 1
#define SQLITE_OS_UNIX 1
#define SQLITE_ENABLE_COLUMN_METADATA 1
