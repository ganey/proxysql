#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include <string>
#include <sstream>
#include <fstream>
#include "mysql.h"

#include "tap.h"
#include "command_line.h"
#include "utils.h"

inline unsigned long long monotonic_time() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (((unsigned long long) ts.tv_sec) * 1000000) + (ts.tv_nsec / 1000);
}

unsigned long calculate_query_execution_time(MYSQL* mysql, const std::string& query) {
	MYSQL_RES *res;
	MYSQL_ROW row;
	unsigned long long begin = monotonic_time();
	unsigned long long row_count = 0;
	MYSQL_QUERY(mysql, query.c_str());
	res = mysql_use_result(mysql);
	unsigned long long num_rows = mysql_num_rows(res);
	unsigned int num_fields = mysql_num_fields(res);
	while ((row = mysql_fetch_row(res))) {
		for (unsigned int i = 1; i < num_fields; i++) {
			char *field = row[i];
		}
		row_count++;
	}
	mysql_free_result(res);	
	unsigned long long end = monotonic_time();
	fprintf(stderr, "Row count: %lld\n", row_count);
	return (end - begin);
}

void set_compression_level(std::string level) {
	CommandLine cl;

	if(cl.getEnv())
		return;

	MYSQL* proxysql = mysql_init(NULL);
	if (!proxysql) {
		fprintf(stderr, "File %s, line %d, Error: %s\n", __FILE__, __LINE__, mysql_error(proxysql));
		return;
	}
	if (!mysql_real_connect(proxysql, cl.host, cl.admin_username, cl.admin_password, NULL, cl.admin_port, NULL, 0)) {
		fprintf(stderr, "File %s, line %d, Error: %s\n", __FILE__, __LINE__, mysql_error(proxysql));
		return;
	}
	std::string query = "UPDATE global_variables SET variable_value = '" + level + "' WHERE variable_name = 'mysql-protocol_compression_level';";
	mysql_query(proxysql, query.c_str());
	query = "LOAD MYSQL VARIABLES TO RUNTIME;";
	mysql_query(proxysql, query.c_str());
	mysql_close(proxysql);
}

MYSQL* initilize_mysql_connection(char* host, char* username, char* password, int port, bool compression) {
	MYSQL* mysql = mysql_init(NULL);
	if (!mysql)
		return nullptr;

	fprintf(stderr, "Connection details: %s %s %d\n", username, password, port);
	if (!mysql_real_connect(mysql, host, username, password, NULL, port, NULL, 0)) {
	    fprintf(stderr, "Failed to connect to database: Error: %s\n",
	              mysql_error(mysql));
		mysql_close(mysql);
		return nullptr;
	}
	if (compression) {
		if (mysql_options(mysql, MYSQL_OPT_COMPRESS, nullptr) != 0) {
			fprintf(stderr, "Failed to set mysql compression option: Error: %s\n",
					mysql_error(mysql));
			mysql_close(mysql);
			return nullptr;
		}
	}
	return mysql;
}

int main(int argc, char** argv) {

	CommandLine cl;

	if(cl.getEnv())
		return exit_status();

	plan(2);

	// ProxySQL connection without compression
	MYSQL* proxysql = initilize_mysql_connection(cl.host, cl.username, cl.password, cl.port, false);
	if (!proxysql) {
		return exit_status();
	}

	// ProxySQL connection with compression
	MYSQL* proxysql_compression = initilize_mysql_connection(cl.host, cl.username, cl.password, cl.port, true);
	if (!proxysql_compression) {
		return exit_status();
	}

	// MySQL connection with compression
	MYSQL* mysql_compression = initilize_mysql_connection(cl.host, cl.username, cl.password, cl.mysql_port, true);
	if (!mysql_compression) {
		return exit_status();
	}

	MYSQL_QUERY(proxysql, "CREATE DATABASE IF NOT EXISTS test");
	MYSQL_QUERY(proxysql, "DROP TABLE IF EXISTS test.sbtest1");

	mysql_query(proxysql, "CREATE TABLE IF NOT EXISTS test.sbtest1 (id INT UNSIGNED NOT NULL AUTO_INCREMENT, k INT UNSIGNED NOT NULL DEFAULT 0, c CHAR(120) NOT NULL DEFAULT '', pad CHAR(60) NOT NULL DEFAULT '', PRIMARY KEY (id), KEY k_1 (k));");

	MYSQL_QUERY(proxysql, "USE test");

	for (int i = 0; i < 1000; i++) {
		MYSQL_QUERY(proxysql, "INSERT INTO sbtest1 (k, c, pad) SELECT FLOOR(RAND() * 10000), REPEAT('a', 120), REPEAT('b', 60) FROM information_schema.tables LIMIT 1000;");
	}

	std::string query = "SELECT t1.id id1, t1.k k1, t1.c c1, t1.pad pad1, t2.id id2, t2.k k2, t2.c c2, t2.pad pad2 FROM test.sbtest1 t1 JOIN test.sbtest1 t2 LIMIT 50000000";

	unsigned long time_proxy = calculate_query_execution_time(proxysql, query);
	diag("Time taken for query with proxysql without compression: %ld", time_proxy);

	unsigned long time_proxy_compressed = calculate_query_execution_time(proxysql_compression, query);
	diag("Time taken for query with proxysql with compression: %ld", time_proxy_compressed);

	unsigned long diff = abs(time_proxy - time_proxy_compressed);
	int performance_diff = (diff * 100) / time_proxy;

	bool accepted_performance = (performance_diff < 10) ? true : false;
	ok((performance_diff < 20), "proxysql with compression performed well compared to without compression. Performance difference: %d percentage", performance_diff);

	unsigned long time_mysql_compressed = calculate_query_execution_time(mysql_compression, query);
	diag("Time taken for query with mysql with compression: %ld", time_mysql_compressed);

	diff = abs(time_mysql_compressed - time_proxy_compressed);
	performance_diff = (diff * 100) / time_mysql_compressed;

	accepted_performance = (performance_diff < 20) ? true : false;
	ok((performance_diff < 10), "proxysql with compression performed well compared to mysql with compression. Performance difference: %d percentage", performance_diff);

	mysql_close(proxysql);
	mysql_close(proxysql_compression);
	mysql_close(mysql_compression);

	return exit_status();
}
