#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Минимальный HTTP/1.1-клиент "скачать один URL целиком в буфер" — то,
 * что стоит за SYS_NET_FETCH (syscall.h). Собирает URL-разбор,
 * dns.c/ip_str_to_addr(), tcp.c (http://) и tls.c (https://), разбор ответа.
 *
 * Урезан: только GET, HTTP/1.1, Connection: close; редиректы (301/302/
 * 307/308) не следуются — возвращаются как обычный результат, Location
 * разбирает вызывающий (dlpg.c); нет userinfo/IPv6-литералов, keep-alive,
 * пайплайнинга, сжатия, кук, аутентификации, условных запросов; тело
 * кладётся в буфер целиком. Поддерживаются Content-Length, chunked и
 * framing "до закрытия соединения".
 */

/*
 * Коды ошибок должны численно совпадать с NET_FETCH_E* из
 * libc/include/lufira/syscall.h — sys_net_fetch() отдаёт результат как
 * есть, dlpg.c сравнивает с этими константами. Менять в одиночку нельзя.
 */
#define HTTP_FETCH_EBADURL  (-1) // не распознан URL (нужна схема http:// или https://)
#define HTTP_FETCH_EDNS     (-2) // не удалось резолвить хост (dns.c)
#define HTTP_FETCH_ECONNECT (-3) // TCP-соединение не установилось (tcp.c)
#define HTTP_FETCH_ETLS     (-4) // TLS-рукопожатие не удалось (tls.c, только https://)
#define HTTP_FETCH_EHTTP    (-5) // ответ сервера не разобрался как HTTP
#define HTTP_FETCH_ENOSPC   (-6) // тело ответа больше out_cap
#define HTTP_FETCH_ENODEV   (-7) // сетевая карта не найдена
#define HTTP_FETCH_EBUSY    (-8) // сеть занята другим fetch'ом дольше бюджета ожидания (net_lock_acquire(), net.h)
#define HTTP_FETCH_ECHUNKED (-9) // потоковый режим (http_fetch_open/read) не поддерживает chunked-тело

// Скачивает url (http:// или https://) целиком в out_buf (до out_cap
// байт). Возвращает длину тела (>=0, <= out_cap) при успехе; status_out
// (может быть NULL) получает HTTP статус-код, если разбор дошёл до
// заголовков, НЕЗАВИСИМО от 2xx/4xx/5xx — 404 это успешно скачанное
// тело с кодом 404, не ошибка. HTTP_FETCH_EHTTP — только для "ответ
// вообще не разобрался как HTTP".
int http_fetch(const char *url, void *out_buf, unsigned long out_cap, int *status_out);

/*
 * Потоковая загрузка одного URL порциями ограниченного размера — для
 * NETD_OP_FETCH_OPEN/READ/CLOSE (sys_net_fetch_open/read/close() в
 * syscall.c), которым нужно скачать файл произвольного размера без
 * буфера на весь файл в RAM (см. PACKAGE_FETCH_CAP в dlpg.c). Соединение
 * (TCP/TLS) держится открытым МЕЖДУ вызовами http_fetch_read() — та же
 * идея единственного соединения, что у http_fetch() выше, только теперь
 * растянутая на несколько IPC-запросов подряд, а не один — иначе каждая
 * порция платила бы за новый TLS-handshake.
 *
 * Использование: ровно один http_fetch_open() (парсит URL, резолвит DNS,
 * коннектится, шлёт запрос, разбирает заголовки до тела), затем сколько
 * угодно http_fetch_read() подряд (каждый — очередная порция тела, 0 —
 * тело кончилось, отрицательное — ошибка), затем ровно один
 * http_fetch_close() (идемпотентен, можно звать и после ошибки/EOF —
 * просто закрывает соединение, если оно ещё открыто).
 *
 * Chunked-тело потоковый режим не поддерживает (HTTP_FETCH_ECHUNKED из
 * http_fetch_open()) — осознанное ограничение: единственный реальный
 * источник (raw.githubusercontent.com) всегда шлёт статический файл с
 * Content-Length, chunked тут взяться не должно; полный разбор chunked
 * уже есть у http_fetch() выше для не-потокового пути (index.json и т.п.).
 */

// status_out (может быть NULL) — HTTP статус, как у http_fetch(). При
// успехе (result==0) *has_length_out установлен в 1, если известна
// Content-Length (её значение — *length_out); иначе 0 и *length_out не
// трогается (тело "до закрытия соединения" — http_fetch_read() сигналит
// конец через conn-close, не через длину).
int http_fetch_open(const char *url, int *status_out, int *has_length_out, unsigned long *length_out);

// Следующая порция тела, до out_cap байт. >0 — сколько реально записано в
// out_buf, 0 — тело полностью получено, <0 — HTTP_FETCH_E* (соединение уже
// закрыто этой функцией на ошибке — http_fetch_close() после неё безвреден).
int http_fetch_read(void *out_buf, unsigned long out_cap);

// Закрывает соединение, открытое http_fetch_open() (если оно ещё открыто).
void http_fetch_close(void);
