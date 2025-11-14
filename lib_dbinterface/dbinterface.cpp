#include "dbinterface.hpp"
#include <iostream>
#include <chrono>
#include <dlfcn.h>
#include <cstring>
#include <iomanip> 
using namespace std;

// Nomes de ConexÃ£o e Tabela
const char* DSN_NAME = "utlogix_sql"; 
const char* TABLE_NAME = "TabelaTeste"; 
const char* username = "totvsvmtests";
const char* password = "totvs@123456";

// DefiniÃ§Ãµes de Handle para ODBC (Inicializados como NULL)
SQLHENV env = SQL_NULL_HENV; 
SQLHDBC dbc = SQL_NULL_HDBC; 
SQLHSTMT stmt = SQL_NULL_HSTMT; 

// Variável estática para simular o estado de conexão (apenas para o exemplo)
static bool isConnected = false;
static bool isODBCLoaded = false;

void* odbc_handle = nullptr;

// Funções para simplificar o cast (SQLPOINTER é geralmente void*)
typedef void* SQLPOINTER;
typedef SQLCHAR *SQLCHAR_PTR;
#define SQL_DRIVER_NAME 6             // Exemplo de InfoType para o nome do driver

// --- Assinaturas Completas das Funções ODBC ---

// 1. SQLAllocHandle (Aloca Handles)
// A função de alocação genérica: SQLAllocHandle(HandleType, InputHandle, OutputHandlePtr)
typedef SQLRETURN (*SQLAllocHandleFunc)(
    SQLSMALLINT HandleType,     // Tipo de Handle a ser alocado (ENV, DBC, STMT)
    SQLHANDLE InputHandle,      // Handle de entrada (ex: Handle do Ambiente para alocar um Handle de Conexão)
    SQLHANDLE *OutputHandlePtr  // Ponteiro para o Handle alocado
);

// 2. SQLSetEnvAttr (Define Atributos do Ambiente)
// SQLSetEnvAttr(EnvironmentHandle, Attribute, ValuePtr, StringLength)
typedef SQLRETURN (*SQLSetEnvAttrFunc)(
    SQLHENV EnvironmentHandle,
    SQLINTEGER Attribute,       // Ex: SQL_ATTR_ODBC_VERSION
    SQLPOINTER ValuePtr,        // O valor a ser definido
    SQLINTEGER StringLength     // Comprimento da string (se ValuePtr for uma string)
);

// 3. SQLSetConnectAttr (Define Atributos da Conexão)
// SQLSetConnectAttr(ConnectionHandle, Attribute, ValuePtr, StringLength)
typedef SQLRETURN (*SQLSetConnectAttrFunc)(
    SQLHDBC ConnectionHandle,
    SQLINTEGER Attribute,
    SQLPOINTER ValuePtr,
    SQLINTEGER StringLength
);

// As Funções Anteriores
typedef SQLRETURN (*SQLConnectFunc)(
    SQLHDBC ConnectionHandle, const SQLCHAR *ServerName, SQLSMALLINT NameLength1, 
    const SQLCHAR *UserName, SQLSMALLINT NameLength2, 
    const SQLCHAR *Authentication, SQLSMALLINT NameLength3
);
typedef SQLRETURN (*SQLDisconnectFunc)(
    SQLHDBC ConnectionHandle
);

// 4. SQLFreeHandle (Libera Handles)
// SQLFreeHandle(HandleType, Handle)
typedef SQLRETURN (*SQLFreeHandleFunc)(
    SQLSMALLINT HandleType,     // Tipo de Handle a ser liberado (ENV, DBC, STMT)
    SQLHANDLE Handle            // O Handle a ser liberado
);

// 5. SQLGetDiagRec (Obtém Registros de Diagnóstico)
// SQLGetDiagRec(HandleType, Handle, RecNumber, Sqlstate, NativeErrorPtr, MessageText, BufferLength, TextLengthPtr)
typedef SQLRETURN (*SQLGetDiagRecFunc)(
    SQLSMALLINT HandleType,             // Tipo de Handle: ENV, DBC, STMT, DESC
    SQLHANDLE Handle,                   // O Handle (hEnv, hDBC, etc.) que gerou o erro
    SQLSMALLINT RecNumber,              // Número do registro de erro a ser buscado (1º, 2º, etc.)
    SQLCHAR *Sqlstate,                  // Buffer de saída para o código SQLSTATE (5 caracteres)
    SQLINTEGER *NativeErrorPtr,         // Buffer de saída para o código de erro nativo do driver/DBMS
    SQLCHAR *MessageText,               // Buffer de saída para a mensagem de erro detalhada
    SQLSMALLINT BufferLength,           // Tamanho máximo do buffer MessageText
    SQLSMALLINT *TextLengthPtr          // Buffer de saída para o comprimento real da mensagem
);

// 6. SQLGetInfo (Obtém Informações do Driver)
// SQLGetInfo(ConnectionHandle, InfoType, InfoValuePtr, BufferLength, StringLengthPtr)
typedef SQLRETURN (*SQLGetInfoFunc)(
    SQLHDBC ConnectionHandle,           // Handle da Conexão (ou Ambiente em alguns casos)
    SQLSMALLINT InfoType,               // O identificador da informação desejada (ex: SQL_DRIVER_NAME)
    SQLPOINTER InfoValuePtr,            // Buffer de saída para o valor da informação
    SQLSMALLINT BufferLength,           // Tamanho do buffer InfoValuePtr
    SQLSMALLINT *StringLengthPtr        // Buffer de saída para o comprimento real dos dados retornados
);

// Variáveis Globais (Ponteiros de Função Mapeados)
SQLAllocHandleFunc _SQLAllocHandle = nullptr;
SQLSetEnvAttrFunc _SQLSetEnvAttr = nullptr;
SQLSetConnectAttrFunc _SQLSetConnectAttr = nullptr;
SQLConnectFunc _SQLConnect = nullptr;
SQLDisconnectFunc _SQLDisconnect = nullptr;
SQLFreeHandleFunc _SQLFreeHandle = nullptr; 
SQLGetDiagRecFunc _SQLGetDiagRec = nullptr;
SQLGetInfoFunc _SQLGetInfo = nullptr; // Novo ponteiro

// =======================================================
// FUNÃ‡Ã•ES AUXILIARES
// =======================================================

void HandleDiagnosticRecord (SQLHANDLE handle, SQLSMALLINT type, const char* functionName) {
    SQLSMALLINT i = 1;
    SQLINTEGER nativeError;
    SQLCHAR state[6], text[256];
    SQLSMALLINT length;
    SQLRETURN ret;

    cerr << "--- Erro em " << functionName << " ---" << endl;
    while ((ret = _SQLGetDiagRec(type, handle, i, state, &nativeError, text, sizeof(text), &length)) == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        cerr << "SQL State: " << state << endl;
        cerr << "Erro Nativo: " << nativeError << endl;
        cerr << "Mensagem: " << text << endl;
        i++;
    }
}

/**
 * @brief Tenta carregar a libodbc.so e mapear as funções.
 * @return true se o mapeamento for bem-sucedido.
 */
bool load_odbc_symbols()
{
  // Se já estiver carregada, retorne sucesso
  if (odbc_handle != nullptr) {
    return true;
  }  

  // Tenta carregar a libodbc.so (que deve estar no LD_LIBRARY_PATH ou caminhos padrão)
  odbc_handle = dlopen("libodbc.so", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND); 

  if (!odbc_handle) {
    // Usa dlerror() para obter a razão exata da falha
    std::cerr << "[DBINTERFACE ERROR] Falha ao carregar libodbc.so: " << dlerror() << "\n";
    return false;
  }

// --- Mapeamento das Novas Funções ---

    _SQLAllocHandle = (SQLAllocHandleFunc)dlsym(odbc_handle, "SQLAllocHandle");
    if (!_SQLAllocHandle) {
        std::cerr << "[ERRO] Falha ao mapear SQLAllocHandle: " << dlerror() << "\n";
        return false;
    }
    std::cout << "[INFO] SQLAllocHandle mapeado.\n";
    
    _SQLSetEnvAttr = (SQLSetEnvAttrFunc)dlsym(odbc_handle, "SQLSetEnvAttr");
    if (!_SQLSetEnvAttr) {
        std::cerr << "[ERRO] Falha ao mapear SQLSetEnvAttr: " << dlerror() << "\n";
        return false;
    }
    std::cout << "[INFO] SQLSetEnvAttr mapeado.\n";

    _SQLSetConnectAttr = (SQLSetConnectAttrFunc)dlsym(odbc_handle, "SQLSetConnectAttr");
    if (!_SQLSetConnectAttr) {
        std::cerr << "[ERRO] Falha ao mapear SQLSetConnectAttr: " << dlerror() << "\n";
        return false;
    }
    std::cout << "[INFO] SQLSetConnectAttr mapeado.\n";
    
    // --- Mapeamento das Funções Anteriores ---

    _SQLConnect = (SQLConnectFunc)dlsym(odbc_handle, "SQLConnect");
    _SQLDisconnect = (SQLDisconnectFunc)dlsym(odbc_handle, "SQLDisconnect");
    
    if (!_SQLConnect || !_SQLDisconnect) {
        std::cerr << "[ERRO] Falha ao mapear SQLConnect/SQLDisconnect.\n";
        return false;
    }
    std::cout << "[INFO] SQLConnect e SQLDisconnect mapeados.\n";

    _SQLFreeHandle = (SQLFreeHandleFunc)dlsym(odbc_handle, "SQLFreeHandle");
    if (!_SQLFreeHandle) {
        std::cerr << "[ERRO] Falha ao mapear SQLFreeHandle: " << dlerror() << "\n";
        return false;
    }
    std::cout << "[INFO] SQLFreeHandle mapeado.\n";  
    
    _SQLGetDiagRec = (SQLGetDiagRecFunc)dlsym(odbc_handle, "SQLGetDiagRec");
    if (!_SQLGetDiagRec) {
        std::cerr << "[ERRO] Falha ao mapear SQLGetDiagRec: " << dlerror() << "\n";
        return false;
    }
    std::cout << "[INFO] SQLGetDiagRec mapeado.\n";    

    _SQLGetInfo = (SQLGetInfoFunc)dlsym(odbc_handle, "SQLGetInfo");
    if (!_SQLGetInfo) {
        std::cerr << "[ERRO] Falha ao mapear SQLGetInfo: " << dlerror() << "\n";
        return false;
    }
    std::cout << "[INFO] SQLGetInfo mapeado.\n";    

  std::cout << "[DBINTERFACE INFO] libodbc.so carregada e funcoes mapeadas com sucesso.\n";
  return true;
}

// Implementação da função de conexão (linkagem C)
int db_connect(const char *dbName) 
{
  SQLRETURN ret;
  string sql;
  int errorCode = 0; // CÃ³digo de erro a retornar

  // VARIÃVEIS MOVIDAS PARA O ESCOPO
  SQLINTEGER id;
  SQLCHAR nome[51];
  SQLCHAR valorStr[51];
  SQLLEN idLen, nomeLen, valorStrLen;
  int rowCount = 0; 

  // --- USANDO SQLGetInfo ---
  SQLCHAR driverName[256];
  SQLSMALLINT actualLength = 0;    

  if (!load_odbc_symbols()) {
    std::cerr << "[DBINTERFACE] Não é possível conectar, falha ao carregar a biblioteca ODBC.\n";
    return 1;
  }  

  // 1. Inicializa o ambiente ODBC
  ret = _SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
  _SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

  // 2. Aloca o handle da conexÃ£o
  ret = _SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
  if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
      errorCode = 1;
      goto cleanup;
  }

  // 3. CONECTA USANDO SQLConnect E DSN
  cout << "Tentando conectar ao DSN '" << DSN_NAME << "' com SQLConnect..." << endl;
  ret = _SQLConnect(dbc, (SQLCHAR*)DSN_NAME, SQL_NTS, 
                    (SQLCHAR*)username, SQL_NTS, 
                    (SQLCHAR*)password, SQL_NTS);

  if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
      HandleDiagnosticRecord(dbc, SQL_HANDLE_DBC, "SQLConnect");
      errorCode = 2;
      goto cleanup;
  }
  cout << "ConexÃ£o estabelecida com sucesso usando DSN!" << endl;
 
  std::cout << "\n--- Buscando Informações do Driver ---\n";

  // Chamando SQLGetInfo para obter o nome do driver
  ret = _SQLGetInfo(
      dbc,
      SQL_DRIVER_NAME, // A informação desejada
      driverName,      // Buffer de saída
      (SQLSMALLINT)sizeof(driverName),
      &actualLength    // Comprimento da string retornada
  );

  if (ret == SQL_SUCCESS) {
      printf("SQLGetInfo(SQL_DRIVER_NAME) Retorno: %d\n",ret);
      // Assegura que a string seja terminada corretamente para impressão
      driverName[actualLength] = '\0';
      std::cout << "Nome do Driver: " << (const char*)driverName << "\n";
  } else {
      HandleDiagnosticRecord(dbc, SQL_HANDLE_DBC, "SQLGetInfo");
      errorCode = 2;
      goto cleanup;
  }  

  // 4. FORÃ‡A O MODO MANUAL-COMMIT
  ret = _SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, SQL_IS_POINTER);
  if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
      HandleDiagnosticRecord(dbc, SQL_HANDLE_DBC, "SQLSetConnectAttr(AUTOCOMMIT_OFF)");
      errorCode = 3;
      goto cleanup;
  }

  // 5. Aloca o handle da instruÃ§Ã£o
  ret = _SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
  if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
      HandleDiagnosticRecord(dbc, SQL_HANDLE_DBC, "SQLAllocHandle(STMT)");
      errorCode = 4;
      goto cleanup;
  }
// Limpeza de Handles e DesconexÃ£o
cleanup:
    cout << "\nEncerrando a conexÃ£o e liberando recursos..." << endl;
    if (stmt != SQL_NULL_HSTMT) _SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    if (dbc != SQL_NULL_HDBC) {
        _SQLDisconnect(dbc); 
        _SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    }
    if (env != SQL_NULL_HENV) _SQLFreeHandle(SQL_HANDLE_ENV, env);

    return errorCode; // Retorna 0 em caso de sucesso
}

// Implementação da função de desconexão (linkagem C)
int db_disconnect() 
{
  if (!load_odbc_symbols()) {
    std::cerr << "[DBINTERFACE] Não é possível conectar, falha ao carregar a biblioteca ODBC.\n";
    return 1;
  } 
  return 0;
}
