#ifndef ODBC_LAUNCHER
#define ODBC_LAUNCHER

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa e executa a rotina completa de conexão ODBC, INSERT e SELECT.
 * * @param username Usuário para conexão com o SQL Server.
 * @param password Senha para conexão com o SQL Server.
 * @return 0 em caso de sucesso, código de erro em caso de falha.
 */
#ifdef USE_DBSRV
int connect_and_run_sql(const char * pServer, const char * pDSN, const char * pUser);
#else
int connect_and_run_sql(const char * pDSN, const char * pUser, const char * pPwd);
#endif
#ifdef __cplusplus
}
#endif

#endif
