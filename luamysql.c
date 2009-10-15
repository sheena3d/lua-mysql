#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef WIN32
#include <winsock2.h>
#define NO_CLIENT_LONG_LONG
#endif

#include "mysql.h"

#include "lua.h"
#include "lauxlib.h"
#if ! defined (LUA_VERSION_NUM) || LUA_VERSION_NUM < 501
#include "compat-5.1.h"
#endif

/* For compat with old version 4.0 */
#if (MYSQL_VERSION_ID < 40100) 
#define MYSQL_TYPE_VAR_STRING   FIELD_TYPE_VAR_STRING 
#define MYSQL_TYPE_STRING       FIELD_TYPE_STRING 
#define MYSQL_TYPE_DECIMAL      FIELD_TYPE_DECIMAL 
#define MYSQL_TYPE_SHORT        FIELD_TYPE_SHORT 
#define MYSQL_TYPE_LONG         FIELD_TYPE_LONG 
#define MYSQL_TYPE_FLOAT        FIELD_TYPE_FLOAT 
#define MYSQL_TYPE_DOUBLE       FIELD_TYPE_DOUBLE 
#define MYSQL_TYPE_LONGLONG     FIELD_TYPE_LONGLONG 
#define MYSQL_TYPE_INT24        FIELD_TYPE_INT24 
#define MYSQL_TYPE_YEAR         FIELD_TYPE_YEAR 
#define MYSQL_TYPE_TINY         FIELD_TYPE_TINY 
#define MYSQL_TYPE_TINY_BLOB    FIELD_TYPE_TINY_BLOB 
#define MYSQL_TYPE_MEDIUM_BLOB  FIELD_TYPE_MEDIUM_BLOB 
#define MYSQL_TYPE_LONG_BLOB    FIELD_TYPE_LONG_BLOB 
#define MYSQL_TYPE_BLOB         FIELD_TYPE_BLOB 
#define MYSQL_TYPE_DATE         FIELD_TYPE_DATE 
#define MYSQL_TYPE_NEWDATE      FIELD_TYPE_NEWDATE 
#define MYSQL_TYPE_DATETIME     FIELD_TYPE_DATETIME 
#define MYSQL_TYPE_TIME         FIELD_TYPE_TIME 
#define MYSQL_TYPE_TIMESTAMP    FIELD_TYPE_TIMESTAMP 
#define MYSQL_TYPE_ENUM         FIELD_TYPE_ENUM 
#define MYSQL_TYPE_SET          FIELD_TYPE_SET
#define MYSQL_TYPE_NULL         FIELD_TYPE_NULL

#define mysql_commit(_) ((void)_)
#define mysql_rollback(_) ((void)_)
#define mysql_autocommit(_,__) ((void)_)

#endif

/* Compatibility between Lua 5.1+ and Lua 5.0 */
#ifndef LUA_VERSION_NUM
#define LUA_VERSION_NUM 0
#endif
#if LUA_VERSION_NUM < 501
#define luaL_register(a, b, c) luaL_openlib((a), (b), (c), 0)
#endif

#define LUA_MYSQL_CONN "MySQL connection"
#define LUA_MYSQL_RES "MySQL result"
#define LUA_MYSQL_TABLENAME "mysql"

typedef struct {
    short      closed;
} pseudo_data;

typedef struct {
    short   closed;
    int     env;
    MYSQL   *conn;
} lua_mysql_conn;

typedef struct {
    short      closed;
    int        conn;               /* reference to connection */
    int        numcols;            /* number of columns */
    int        colnames, coltypes; /* reference to column information tables */
    MYSQL_RES *res;
} lua_mysql_res;

/**
** common functions Part
*/

/*
** Get the internal database type of the given column.
*/
static char *luaM_getcolumntype (enum enum_field_types type) {

    switch (type) {
        case MYSQL_TYPE_VAR_STRING: case MYSQL_TYPE_STRING:
            return "string";
        case MYSQL_TYPE_DECIMAL: case MYSQL_TYPE_SHORT: case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_FLOAT: case MYSQL_TYPE_DOUBLE: case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24: case MYSQL_TYPE_YEAR: case MYSQL_TYPE_TINY:
            return "number";
        case MYSQL_TYPE_TINY_BLOB: case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: case MYSQL_TYPE_BLOB:
            return "binary";
        case MYSQL_TYPE_DATE: case MYSQL_TYPE_NEWDATE:
            return "date";
        case MYSQL_TYPE_DATETIME:
            return "datetime";
        case MYSQL_TYPE_TIME:
            return "time";
        case MYSQL_TYPE_TIMESTAMP:
            return "timestamp";
        case MYSQL_TYPE_ENUM: case MYSQL_TYPE_SET:
            return "set";
        case MYSQL_TYPE_NULL:
            return "null";
        default:
            return "undefined";
    }
}

/*                   
** Return the name of the object's metatable.
** This function is used by `tostring'.     
*/                            
static int luaM_tostring (lua_State *L) {                
    char buff[100];             
    pseudo_data *obj = (pseudo_data *)lua_touserdata (L, 1);     
    if (obj->closed)                          
        strcpy (buff, "closed");
    else
        sprintf (buff, "%p", (void *)obj);
    lua_pushfstring (L, "%s (%s)", lua_tostring(L,lua_upvalueindex(1)), buff);
    return 1;                            
}       

/*
** Define the metatable for the object on top of the stack
*/
void luaM_setmeta (lua_State *L, const char *name) {
    luaL_getmetatable (L, name);
    lua_setmetatable (L, -2);
}     

/*
** Create a metatable and leave it on top of the stack.
*/
int luaM_register (lua_State *L, const char *name, const luaL_reg *methods) {
    if (!luaL_newmetatable (L, name))
        return 0;

    /* define methods */
    luaL_register (L, NULL, methods);

    /* define metamethods */
    lua_pushliteral (L, "__gc");
    lua_pushcfunction (L, methods->func);
    lua_settable (L, -3);

    lua_pushliteral (L, "__index");
    lua_pushvalue (L, -2);
    lua_settable (L, -3);

    lua_pushliteral (L, "__tostring");
    lua_pushstring (L, name);
    lua_pushcclosure (L, luaM_tostring, 1);
    lua_settable (L, -3);

    lua_pushliteral (L, "__metatable");
    lua_pushliteral (L, "you're not allowed to get this metatable");
    lua_settable (L, -3);

    return 1;
}

/*
** message
*/

static int luaM_msg(lua_State *L, const int n, const char *m) {
    if (n) {
        lua_pushnumber(L, 1);
    } else {
        lua_pushnil(L);
    }
    lua_pushstring(L, m);
    return 2;
}

/*
** Push the value of #i field of #tuple row.
*/
static void luaM_pushvalue (lua_State *L, void *row, long int len) {
    if (row == NULL)
        lua_pushnil (L);
    else
        lua_pushlstring (L, row, len);
}

/*
** Creates the lists of fields names and fields types.
*/
static void luaM_colinfo (lua_State *L, lua_mysql_res *my_res) {
    MYSQL_FIELD *fields;
    char typename[50];
    int i;
    fields = mysql_fetch_fields(my_res->res);
    lua_newtable (L); /* names */
    lua_newtable (L); /* types */
    for (i = 1; i <= my_res->numcols; i++) {
        lua_pushstring (L, fields[i-1].name);
        lua_rawseti (L, -3, i);
        sprintf (typename, "%.20s(%ld)", luaM_getcolumntype (fields[i-1].type), fields[i-1].length);
        lua_pushstring(L, typename);
        lua_rawseti (L, -2, i);
    }
    /* Stores the references in the cursor structure */
    my_res->coltypes = luaL_ref (L, LUA_REGISTRYINDEX);
    my_res->colnames = luaL_ref (L, LUA_REGISTRYINDEX);
}


/**
** Handle Part
*/

/*
** Check for valid connection.
*/
static lua_mysql_conn *Mget_conn (lua_State *L) {
    lua_mysql_conn *my_conn = (lua_mysql_conn *)luaL_checkudata (L, 1, LUA_MYSQL_CONN);
    luaL_argcheck (L, my_conn != NULL, 1, "connection expected");
    luaL_argcheck (L, !my_conn->closed, 1, "connection is closed");
    return my_conn;
}

/*
** Check for valid result.
*/
static lua_mysql_res *Mget_res (lua_State *L) {
    lua_mysql_res *my_res = (lua_mysql_res *)luaL_checkudata (L, 1, LUA_MYSQL_RES);
    luaL_argcheck (L, my_res != NULL, 1, "result expected");
    luaL_argcheck (L, !my_res->closed, 1, "result is closed");
    return my_res;
}

/*
** MYSQL operate functions
*/

/**
** Open a connection to a MySQL Server
*/
static int Lmysql_connect (lua_State *L) {
    lua_mysql_conn *my_conn = (lua_mysql_conn *)lua_newuserdata(L, sizeof(lua_mysql_conn));
    luaM_setmeta (L, LUA_MYSQL_CONN);

    char *host = NULL, *delim = ":";
    const char *host_and_port = luaL_optstring(L, 1, NULL);
    const char *user = luaL_optstring(L, 2, NULL);
    const char *passwd = luaL_optstring(L, 3, NULL);

    int port = MYSQL_PORT;
    
    MYSQL *conn;

    conn = mysql_init(NULL);  
    if ( ! conn) {  
        return luaM_msg (L, 0, "Error: mysql_init failed !");
    }

    host = strtok(host_and_port, delim);
    port = strtok(NULL, delim);

    if ( ! mysql_real_connect(conn, host, user, passwd, NULL, port, NULL, 0)) {
        mysql_close (conn); /* Close conn if connect failed */
        return luaM_msg (L, 0, mysql_error(conn));
    }

    /* fill in structure */
    my_conn->closed = 0;
    my_conn->env = LUA_NOREF;
    my_conn->conn = conn;

    return 1;
}

/**
** Select a MySQL database
*/
static int Lmysql_select_db (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    const char *db = luaL_checkstring (L, 2);

    if (mysql_select_db(my_conn->conn, db) != 0) {
        return luaM_msg (L, 0, mysql_error(my_conn->conn));
    }
    else {
        lua_pushboolean(L, 1);
        return 1;
    }
}

/**
** Sets the client character set
*/
static int Lmysql_set_charset (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    const char *charset = luaL_checkstring (L, 2);
    const char statement1[1024];

    /* major_version*10000 + minor_version *100 + sub_version
      For example, 5.1.5 is returned as 50105. 
    */
    unsigned long version = mysql_get_server_version(my_conn->conn);

    /* set charset */
    if ( version > 41000) {
        sprintf(statement1, "SET character_set_connection=%s, character_set_results=%s, character_set_client=binary", charset, charset);
        unsigned long st_len1 = strlen(statement1);
        if (mysql_real_query(my_conn->conn, statement1, st_len1)) {
            return luaM_msg (L, 0, mysql_error(my_conn->conn));
        }
    }
    else {
        if (mysql_set_character_set(my_conn->conn, charset)) {
            return luaM_msg (L, 0, mysql_error(my_conn->conn));
        }
    }

    if ( version > 50001) {
        const char *statement2 = "SET sql_mode=''";
        unsigned long st_len2 = strlen(statement2);
        if (mysql_real_query(my_conn->conn, statement2, st_len2)) {
            return luaM_msg (L, 0, mysql_error(my_conn->conn));
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

/**
** Returns the text of the error message from previous MySQL operation
*/
static int Lmysql_error (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    lua_pushstring(L, mysql_error(my_conn->conn));
    return 1;
}

/**
** Returns the numerical value of the error message from previous MySQL operation
*/
static int Lmysql_errno (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    lua_pushnumber(L, mysql_errno(my_conn->conn));
    return 1;
}

/**
** Returns the version number of the server as an integer
** major_version*10000 + minor_version *100 + sub_version
** For example, 5.1.5 is returned as 50105. 
*/
static int Lmysql_get_server_version (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    lua_pushnumber(L, mysql_get_server_version(my_conn->conn));
    return 1;
}

/**
** Returns a string that represents the server version number. 
*/
static int Lmysql_get_server_info (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    lua_pushstring(L, mysql_get_server_info(my_conn->conn));
    return 1;
}

/**
**  Get number of affected rows in previous MySQL operation
*/
static int Lmysql_affected_rows (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    lua_pushnumber(L, mysql_affected_rows(my_conn->conn));
    return 1;
}

/**
** Send a MySQL query
*/
static int Lmysql_query (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    const char *statement = luaL_checkstring (L, 2);
    unsigned long st_len = strlen(statement);

    if (mysql_real_query(my_conn->conn, statement, st_len)) {
        /* error executing query */
        return luaM_msg (L, 0, mysql_error(my_conn->conn));
    }
    else
    {
        MYSQL_RES *res = mysql_store_result(my_conn->conn);
        unsigned int num_cols = mysql_field_count(my_conn->conn);

        if (res) { /* tuples returned */
            lua_mysql_res *my_res = (lua_mysql_res *)lua_newuserdata(L, sizeof(lua_mysql_res));
            luaM_setmeta (L, LUA_MYSQL_RES);

            /* fill in structure */
            my_res->closed = 0;
            my_res->conn = LUA_NOREF;
            my_res->numcols = num_cols;
            my_res->colnames = LUA_NOREF;
            my_res->coltypes = LUA_NOREF;
            my_res->res = res;
            lua_pushvalue (L, my_conn);
            my_res->conn = luaL_ref (L, LUA_REGISTRYINDEX);

            return 1;
        }
        else { /* mysql_use_result() returned nothing; should it have? */
            if (num_cols == 0) { /* no tuples returned */
                /* query does not return data (it was not a SELECT) */
                lua_pushnumber(L, mysql_affected_rows(my_conn->conn));
                return 1;
            }
            else { /* mysql_use_result() should have returned data */
                return luaM_msg (L, 0, mysql_error(my_conn->conn));
            }
        }
    }
}

/**
** Get the ID generated from the previous INSERT operation
*/
static int Lmysql_insert_id (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    lua_pushnumber(L, mysql_insert_id(my_conn->conn));
    return 1;
}

/**
** Escapes special characters in a string for use in a SQL statement
*/
static int Lmysql_real_escape_string (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    const char *unescaped_string = luaL_checkstring (L, 2);
    unsigned long st_len = strlen(unescaped_string);
    char to[st_len*2+1]; 
    mysql_real_escape_string(my_conn->conn, to, unescaped_string, st_len);
    lua_pushstring(L, to);
    return 1;
}

/**
** Fetch a result row as an associative array, a numeric array, or both
*/
static int Lmysql_fetch_array (lua_State *L) {
    lua_mysql_res *my_res = Mget_res (L);
    MYSQL_RES *res = my_res->res;
    unsigned long *lengths;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row == NULL) {
        lua_pushnil(L);  /* no more results */
        return 1;
    }

    lengths = mysql_fetch_lengths(res);
    if (lua_istable (L, 2)) {
        const char *opts = luaL_optstring (L, 3, "n");
        if (strchr (opts, 'n') != NULL) {
            /* Copy values to numerical indices */
            int i;
            for (i = 0; i < my_res->numcols; i++) {
                luaM_pushvalue (L, row[i], lengths[i]);
                lua_rawseti (L, 2, i+1);
            }
        }
        if (strchr (opts, 'a') != NULL) {
            int i;
            /* Check if colnames exists */
            if (my_res->colnames == LUA_NOREF)
                luaM_colinfo(L, my_res);
            lua_rawgeti (L, LUA_REGISTRYINDEX, my_res->colnames);/* Push colnames*/

            /* Copy values to alphanumerical indices */
            for (i = 0; i < my_res->numcols; i++) {
                lua_rawgeti(L, -1, i+1); /* push the field name */

                /* Actually push the value */
                luaM_pushvalue (L, row[i], lengths[i]);
                lua_rawset (L, 2);
            }
            /* lua_pop(L, 1);  Pops colnames table. Not needed */
        }
        lua_pushvalue(L, 2);
        return 1; /* return table */
    }
    else {
        int i;
        luaL_checkstack (L, my_res->numcols, "too many columns");
        for (i = 0; i < my_res->numcols; i++)
            luaM_pushvalue (L, row[i], lengths[i]);
        return my_res->numcols; /* return #numcols values */
    }
}

/*
** Rollback the current transaction.
*/
static int Lmysql_rollback (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    if (mysql_rollback(my_conn->conn)) {
        return luaM_msg (L, 0, mysql_error(my_conn->conn));
    }
    return 0;
}

/**
** Free result memory
*/
static int Lmysql_free_result (lua_State *L) {
    lua_mysql_res *my_res = (lua_mysql_res *)luaL_checkudata (L, 1, LUA_MYSQL_RES);
    luaL_argcheck (L, my_res != NULL, 1, "result expected");
    if (my_res->closed) {
        lua_pushboolean (L, 0);
        return 1;
    }

    /* Nullify structure fields. */
    my_res->closed = 1;
    mysql_free_result(my_res->res);
    luaL_unref (L, LUA_REGISTRYINDEX, my_res->conn);
    luaL_unref (L, LUA_REGISTRYINDEX, my_res->colnames);
    luaL_unref (L, LUA_REGISTRYINDEX, my_res->coltypes);

    lua_pushboolean (L, 1);

    return 1;
}

/**
 Close MySQL connection
*/
static int Lmysql_close (lua_State *L) {
    lua_mysql_conn *my_conn = Mget_conn (L);
    luaL_argcheck (L, my_conn != NULL, 1, "connection expected");
    if (my_conn->closed) {
        lua_pushboolean (L, 0);
        return 1;
    }

    my_conn->closed = 1;
    luaL_unref (L, LUA_REGISTRYINDEX, my_conn->env);
    mysql_close (my_conn->conn);
    lua_pushboolean (L, 1);
    return 1;
}


/*
** Creates the metatables for the objects and registers the
** driver open method.
*/
int luaopen_mysql (lua_State *L) {
    struct luaL_reg driver[] = {
        { "connect",   Lmysql_connect },
        { NULL, NULL },
    };

    struct luaL_reg result_methods[] = {
        { "fetch_array",   Lmysql_fetch_array },
        { "free_result",   Lmysql_free_result },
        { NULL, NULL }
    };

    static const luaL_reg connection_methods[] = {
        { "connect",   Lmysql_connect },
        { "error",   Lmysql_error },
        { "errno",   Lmysql_errno },
        { "select_db",   Lmysql_select_db },
        { "insert_id",   Lmysql_insert_id },
        { "set_charset",   Lmysql_set_charset },
        { "affected_rows",   Lmysql_affected_rows },
        { "get_server_info",   Lmysql_get_server_info },
        { "get_server_version",   Lmysql_get_server_version },
        { "real_escape_string",   Lmysql_real_escape_string },
        { "escape_string",   Lmysql_real_escape_string },
        { "query",   Lmysql_query },
        { "rollback",   Lmysql_rollback },
        { "close",   Lmysql_close },
        { NULL, NULL }
    };

    luaM_register (L, LUA_MYSQL_CONN, connection_methods);
    luaM_register (L, LUA_MYSQL_RES, result_methods);
    lua_pop (L, 2);

    luaL_register (L, LUA_MYSQL_TABLENAME, driver);

    lua_pushliteral (L, "_MYSQLVERSION");
    lua_pushliteral (L, MYSQL_SERVER_VERSION);   
    lua_settable (L, -3);     

    return 1;
}

