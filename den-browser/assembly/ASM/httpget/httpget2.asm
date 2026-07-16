; ============================================================
; httpget.asm - minimal HTTP client in x86-64 asm, now with DNS.
; Raw syscalls only: socket, connect, sendto, recvfrom, write,
; read, open, close. No libc.
;
; DNS resolution reads /etc/resolv.conf for a nameserver (falling
; back to 8.8.8.8), builds a raw DNS query packet by hand, sends
; it over a UDP socket, and parses the first A record out of the
; response. That's "the default Linux network path" in the sense
; of using the system's configured resolver - but the actual DNS
; wire protocol is hand-rolled here, no getaddrinfo/libc involved.
;
; Build:
;   nasm -f elf64 httpget.asm -o httpget.o
;   ld httpget.o -o httpget
;
; Usage:
;   ./httpget <host> [path] [port]
;
;   ./httpget example.com /
;   ./httpget 93.184.216.34 / 80
;
; <host> can be a dotted IPv4 address (skips DNS entirely) or a
; hostname (resolved via UDP DNS first). Sends a plain HTTP/1.1
; GET with Connection: close, dumps the raw response straight to
; stdout, unparsed.
; ============================================================

section .data

req_get:        db "GET "
req_get_len     equ $ - req_get
req_httpver:    db " HTTP/1.1", 13, 10
req_httpver_len equ $ - req_httpver
req_host:       db "Host: "
req_host_len    equ $ - req_host
req_conn:       db 13, 10, "Connection: close", 13, 10, 13, 10
req_conn_len    equ $ - req_conn

usage_msg:      db "usage: httpget <host> [path] [port]", 10
usage_len       equ $ - usage_msg

sock_err_msg:   db "httpget: socket() failed", 10
sock_err_len    equ $ - sock_err_msg

conn_err_msg:   db "httpget: connect() failed", 10
conn_err_len    equ $ - conn_err_msg

dns_err_msg:    db "httpget: dns resolution failed", 10
dns_err_len     equ $ - dns_err_msg

default_path:   db "/", 0

resolv_path:        db "/etc/resolv.conf", 0
nameserver_prefix:   db "nameserver "
nameserver_prefix_len equ $ - nameserver_prefix
fallback_dns:        db "8.8.8.8", 0
cloudflare_dns:      db "1.1.1.1", 0

; DNS header: ID=0x1234, flags=standard recursive query,
; QDCOUNT=1, AN/NS/AR COUNT=0
dns_query_header:     db 0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
dns_query_header_len  equ $ - dns_query_header
; QTYPE=A(1), QCLASS=IN(1)
dns_qtype_class:      db 0x00, 0x01, 0x00, 0x01
dns_qtype_class_len   equ $ - dns_qtype_class

section .bss

request_buf:    resb 1024
recv_buf:       resb 65536
sockaddr_in:    resb 16          ; family(2) port(2) addr(4) pad(8)

resolv_buf:         resb 2048
resolver_ip_str:    resb 64
dns_query_buf:      resb 512
dns_resp_buf:       resb 2048
resolver_sockaddr:  resb 16

section .text
global _start

; ============================================================
; _start
; argv: [1]=host_or_ip  [2]=path (optional)  [3]=port (optional)
; ============================================================
_start:
    mov r15, [rsp]                ; argc
    cmp r15, 2
    jl  .usage

    mov r12, [rsp+16]             ; argv[1] = host or ip
    mov r13, r12                  ; same string is used for Host:

    cmp r15, 3
    jl  .no_path
    mov r14, [rsp+24]             ; argv[2] = path
    jmp .have_path
.no_path:
    lea r14, [default_path]
.have_path:

    ; --- optional port argv[3], default 80 ---
    mov r15w, 80
    cmp qword [rsp], 4
    jl  .no_port
    mov rdi, [rsp+32]             ; argv[3] = port string
    call parse_uint16
    mov r15w, ax
.no_port:

    ; --- resolve host: literal dotted IPv4 first, else DNS ---
    mov rdi, r12
    call parse_ipv4
    cmp rax, 0
    jl  .try_dns
    mov r8, rax
    jmp .have_addr
.try_dns:
    mov rdi, r12
    call resolve_dns
    cmp rax, 0
    jl  .dns_error
    mov r8, rax
.have_addr:

    ; --- build sockaddr_in ---
    mov ax, r15w
    xchg al, ah
    mov r15w, ax

    lea rbx, [sockaddr_in]
    mov word [rbx], 2             ; AF_INET
    mov [rbx+2], r15w             ; port, network byte order
    mov [rbx+4], r8d              ; addr, network byte order
    mov qword [rbx+8], 0

    ; --- socket(AF_INET, SOCK_STREAM, 0) ---
    mov rdi, 2
    mov rsi, 1
    xor rdx, rdx
    mov rax, 41
    syscall
    cmp rax, 0
    jl  .sock_error
    mov r9, rax

    ; --- connect ---
    mov rdi, r9
    lea rsi, [sockaddr_in]
    mov rdx, 16
    mov rax, 42
    syscall
    cmp rax, 0
    jl  .conn_error

    ; --- build the request ---
    lea rdi, [request_buf]

    lea rsi, [req_get]
    mov rcx, req_get_len
    call membuf_copy

    mov rsi, r14
    call strlen_copy

    lea rsi, [req_httpver]
    mov rcx, req_httpver_len
    call membuf_copy

    lea rsi, [req_host]
    mov rcx, req_host_len
    call membuf_copy

    mov rsi, r13
    call strlen_copy

    lea rsi, [req_conn]
    mov rcx, req_conn_len
    call membuf_copy

    lea rax, [request_buf]
    sub rdi, rax
    mov rbp, rdi

    ; --- write the request ---
    mov rdi, r9
    lea rsi, [request_buf]
    mov rdx, rbp
    mov rax, 1
    syscall

    ; --- read loop: dump response to stdout ---
.read_loop:
    mov rdi, r9
    lea rsi, [recv_buf]
    mov rdx, 65536
    mov rax, 0
    syscall
    cmp rax, 0
    jle .done

    mov rbx, rax
    mov rdi, 1
    lea rsi, [recv_buf]
    mov rdx, rbx
    mov rax, 1
    syscall
    jmp .read_loop

.done:
    mov rdi, r9
    mov rax, 3
    syscall

    xor rdi, rdi
    mov rax, 60
    syscall

.usage:
    lea rsi, [usage_msg]
    mov rdx, usage_len
    mov rdi, 1
    mov rax, 1
    syscall
    mov rdi, 1
    mov rax, 60
    syscall

.sock_error:
    lea rsi, [sock_err_msg]
    mov rdx, sock_err_len
    mov rdi, 1
    mov rax, 1
    syscall
    mov rdi, 1
    mov rax, 60
    syscall

.conn_error:
    lea rsi, [conn_err_msg]
    mov rdx, conn_err_len
    mov rdi, 1
    mov rax, 1
    syscall
    mov rdi, 1
    mov rax, 60
    syscall

.dns_error:
    lea rsi, [dns_err_msg]
    mov rdx, dns_err_len
    mov rdi, 1
    mov rax, 1
    syscall
    mov rdi, 1
    mov rax, 60
    syscall

; ============================================================
; membuf_copy: rdi=dest, rsi=src, rcx=len -> copies, advances rdi
; ============================================================
membuf_copy:
    push rcx
    cld
    rep movsb
    pop rcx
    ret

; ============================================================
; strlen_copy: rdi=dest, rsi=src (null-terminated) -> copies,
; advances rdi past the copied bytes (no null terminator written)
; ============================================================
strlen_copy:
    push rax
.slc_loop:
    mov al, [rsi]
    cmp al, 0
    je  .slc_done
    mov [rdi], al
    inc rsi
    inc rdi
    jmp .slc_loop
.slc_done:
    pop rax
    ret

; ============================================================
; mem_compare: rdi=ptr1, rsi=ptr2, rdx=len -> rax=1 if equal else 0
; ============================================================
mem_compare:
    push rbx
    xor rcx, rcx
.cmp_loop:
    cmp rcx, rdx
    jge .cmp_equal
    mov al, [rdi+rcx]
    mov bl, [rsi+rcx]
    cmp al, bl
    jne .cmp_not_equal
    inc rcx
    jmp .cmp_loop
.cmp_equal:
    mov rax, 1
    pop rbx
    ret
.cmp_not_equal:
    xor rax, rax
    pop rbx
    ret

; ============================================================
; parse_uint16: rdi = pointer to a null-terminated decimal string
; returns: ax = parsed value (host byte order), 0 on empty/junk
; ============================================================
parse_uint16:
    xor rax, rax
.puint_loop:
    movzx rdx, byte [rdi]
    cmp rdx, 0
    je  .puint_done
    cmp rdx, '0'
    jl  .puint_done
    cmp rdx, '9'
    jg  .puint_done
    sub rdx, '0'
    imul rax, rax, 10
    add rax, rdx
    inc rdi
    jmp .puint_loop
.puint_done:
    ret

; ============================================================
; parse_ipv4: rdi = pointer to null-terminated dotted-decimal
; string, e.g. "93.184.216.34"
; returns: rax = 32-bit address in network byte order, or
;          rax = -1 on parse failure
; Clobbers: rcx, rdx, r8, r9, r10, r11
; ============================================================
parse_ipv4:
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor rcx, rcx

.pip_loop:
    movzx rax, byte [rdi]
    cmp al, 0
    je  .pip_end_of_string
    cmp al, '.'
    je  .pip_dot
    cmp al, '0'
    jl  .pip_fail
    cmp al, '9'
    jg  .pip_fail

    sub al, '0'
    movzx rdx, al
    imul r9, r9, 10
    add r9, rdx
    inc rcx
    cmp rcx, 3
    jg  .pip_fail
    cmp r9, 255
    jg  .pip_fail

    inc rdi
    jmp .pip_loop

.pip_dot:
    cmp rcx, 0
    je  .pip_fail
    cmp r8, 3
    jge .pip_fail

    mov r11, r9
    mov rax, r8
    imul rax, rax, 8
    mov cl, al
    shl r11, cl
    or  r10, r11

    inc r8
    xor r9, r9
    xor rcx, rcx
    inc rdi
    jmp .pip_loop

.pip_end_of_string:
    cmp rcx, 0
    je  .pip_fail
    cmp r8, 3
    jne .pip_fail

    mov rax, r8
    imul rax, rax, 8
    mov cl, al
    mov r11, r9
    shl r11, cl
    or  r10, r11

    mov rax, r10
    ret

.pip_fail:
    mov rax, -1
    ret

; ============================================================
; encode_qname: rdi=dest, rsi=src (dotted hostname, null-term)
; Writes DNS length-prefixed labels ending in a zero byte.
; Advances rdi past the terminating zero.
; Clobbers: rax, rcx, r8
; ============================================================
encode_qname:
.next_label:
    mov r8, rdi                   ; remember length-byte slot
    inc rdi
    xor rcx, rcx
.copy_chars:
    mov al, [rsi]
    cmp al, 0
    je  .label_end
    cmp al, '.'
    je  .label_end
    mov [rdi], al
    inc rdi
    inc rsi
    inc rcx
    jmp .copy_chars
.label_end:
    mov [r8], cl
    cmp al, 0
    je  .qname_done
    inc rsi                       ; skip the dot
    jmp .next_label
.qname_done:
    mov byte [rdi], 0
    inc rdi
    ret

; ============================================================
; resolve_dns: rdi = hostname (null-terminated)
; returns: rax = resolved IPv4 addr (network byte order), or
;          rax = -1 on any failure
; ============================================================
resolve_dns:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r12, rdi                  ; keep hostname safe across calls

    ; --- try to read /etc/resolv.conf for "nameserver X" ---
    lea rdi, [resolv_path]
    xor rsi, rsi
    xor rdx, rdx
    mov rax, 2                    ; open
    syscall
    cmp rax, 0
    jl  .use_fallback_dns
    mov r13, rax                  ; fd

    lea rsi, [resolv_buf]
    mov rdx, 2047
    mov rdi, r13
    xor rax, rax                  ; read
    syscall
    mov r14, rax                  ; bytes read

    mov rdi, r13
    mov rax, 3                    ; close
    syscall

    cmp r14, 0
    jle .use_fallback_dns

    mov byte [resolv_buf + r14], 0

    ; scan for "nameserver " prefix, byte-index in rcx
    xor rcx, rcx
.scan_loop:
    mov rax, r14
    sub rax, rcx
    cmp rax, nameserver_prefix_len
    jl  .use_fallback_dns          ; ran off the end, never found it

    push rcx
    lea rdi, [resolv_buf]
    add rdi, rcx
    lea rsi, [nameserver_prefix]
    mov rdx, nameserver_prefix_len
    call mem_compare
    pop rcx
    cmp rax, 1
    je  .found_prefix
    inc rcx
    jmp .scan_loop

.found_prefix:
    lea rsi, [resolv_buf]
    add rsi, rcx
    add rsi, nameserver_prefix_len
    lea rdi, [resolver_ip_str]
    xor r15, r15
.copy_ip_loop:
    cmp r15, 63
    jge .copy_ip_done
    mov al, [rsi+r15]
    cmp al, 0
    je  .copy_ip_done
    cmp al, 10
    je  .copy_ip_done
    cmp al, 13
    je  .copy_ip_done
    cmp al, ' '
    je  .copy_ip_done
    mov [rdi+r15], al
    inc r15
    jmp .copy_ip_loop
.copy_ip_done:
    mov byte [rdi+r15], 0
    jmp .have_resolver_ip

.use_fallback_dns:
    lea rsi, [fallback_dns]
    lea rdi, [resolver_ip_str]
    call strlen_copy
    mov byte [rdi], 0

.have_resolver_ip:
    ; --- attempt 1: whatever's in resolver_ip_str (system resolver
    ; or the 8.8.8.8 fallback if resolv.conf was missing/empty) ---
    lea rsi, [resolver_ip_str]
    mov rdi, r12
    call query_dns_server
    cmp rax, 0
    jge .resolve_done              ; got an address, done

    ; --- attempt 2: Cloudflare, only because attempt 1 failed ---
    lea rsi, [cloudflare_dns]
    mov rdi, r12
    call query_dns_server
    cmp rax, 0
    jl  .dns_fail

.resolve_done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.dns_fail:
    mov rax, -1
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; ============================================================
; query_dns_server: rdi = hostname (null-terminated),
;                   rsi = resolver IPv4 address (null-terminated
;                   dotted string, e.g. "8.8.8.8")
; Sends one DNS query to that server and parses the first A
; record out of the reply.
; returns: rax = resolved IPv4 addr (network byte order), or
;          rax = -1 on any failure (bad resolver ip, no reply,
;          no A record found, etc.)
; ============================================================
query_dns_server:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r12, rdi                   ; hostname (keep safe across calls)

    mov rdi, rsi                   ; resolver ip string
    call parse_ipv4
    cmp rax, 0
    jl  .qds_fail
    mov r9d, eax                   ; resolver addr

    ; --- build resolver sockaddr (UDP port 53) ---
    lea rbx, [resolver_sockaddr]
    mov word [rbx], 2
    mov word [rbx+2], 0x3500       ; port 53, network order
    mov [rbx+4], r9d
    mov qword [rbx+8], 0

    ; --- build the DNS query packet ---
    lea rdi, [dns_query_buf]
    lea rsi, [dns_query_header]
    mov rcx, dns_query_header_len
    call membuf_copy

    mov rsi, r12
    call encode_qname

    lea rsi, [dns_qtype_class]
    mov rcx, dns_qtype_class_len
    call membuf_copy

    lea rax, [dns_query_buf]
    mov r13, rdi
    sub r13, rax                   ; r13 = total query length

    ; --- UDP socket ---
    mov rdi, 2                     ; AF_INET
    mov rsi, 2                     ; SOCK_DGRAM
    xor rdx, rdx
    mov rax, 41                    ; socket
    syscall
    cmp rax, 0
    jl  .qds_fail
    mov r12, rax                    ; udp fd (r12 is free now; r11 is
                                     ; NOT safe here since `syscall`
                                     ; clobbers rcx/r11 as a side effect)

    ; --- sendto(fd, query, len, 0, &resolver_sockaddr, 16) ---
    mov rdi, r12
    lea rsi, [dns_query_buf]
    mov rdx, r13
    xor r10, r10
    lea r8, [resolver_sockaddr]
    mov r9, 16
    mov rax, 44                    ; sendto
    syscall
    cmp rax, 0
    jl  .qds_fail_close

    ; --- recvfrom(fd, resp, size, 0, NULL, NULL) ---
    mov rdi, r12
    lea rsi, [dns_resp_buf]
    mov rdx, 2047
    xor r10, r10
    xor r8, r8
    xor r9, r9
    mov rax, 45                    ; recvfrom
    syscall
    cmp rax, 0
    jle .qds_fail_close

    mov rdi, r12
    mov rax, 3                     ; close udp socket
    syscall

    ; --- parse the response ---
    lea rbx, [dns_resp_buf]

    ; ANCOUNT is header bytes 6,7 (big-endian)
    movzx rax, byte [rbx+6]
    shl rax, 8
    movzx rdx, byte [rbx+7]
    or  rax, rdx
    mov r14, rax                   ; ANCOUNT
    cmp r14, 0
    jle .qds_fail

    ; skip 12-byte header, then the question's QNAME + QTYPE/QCLASS
    mov r15, 12
.qds_skip_qname_loop:
    movzx rax, byte [rbx+r15]
    cmp rax, 0
    je  .qds_qname_done
    inc r15
    add r15, rax
    jmp .qds_skip_qname_loop
.qds_qname_done:
    inc r15
    add r15, 4                     ; QTYPE + QCLASS

    mov rcx, r14                   ; answer counter
.qds_answer_loop:
    cmp rcx, 0
    jle .qds_fail

    ; NAME field: compressed pointer (top 2 bits = 11) or inline labels
    movzx rax, byte [rbx+r15]
    mov rdx, rax
    and rdx, 0xC0
    cmp rdx, 0xC0
    jne .qds_name_inline
    add r15, 2
    jmp .qds_name_done
.qds_name_inline:
.qds_skip_name_inline_loop:
    movzx rax, byte [rbx+r15]
    cmp rax, 0
    je  .qds_name_inline_done
    inc r15
    add r15, rax
    jmp .qds_skip_name_inline_loop
.qds_name_inline_done:
    inc r15
.qds_name_done:

    ; TYPE (2 bytes, big-endian)
    movzx rax, byte [rbx+r15]
    shl rax, 8
    movzx rdx, byte [rbx+r15+1]
    or  rax, rdx
    push rax                        ; stash the record type
    add r15, 2

    add r15, 2                      ; CLASS
    add r15, 4                      ; TTL

    ; RDLENGTH (2 bytes, big-endian)
    movzx rax, byte [rbx+r15]
    shl rax, 8
    movzx rdx, byte [rbx+r15+1]
    or  rax, rdx
    mov r8, rax                      ; rdlength
    add r15, 2

    pop rdx                          ; record type
    cmp rdx, 1                       ; TYPE A?
    jne .qds_skip_rdata
    cmp r8, 4
    jne .qds_skip_rdata

    ; found it: 4 raw address bytes, already in the right byte
    ; order for sockaddr_in (network byte order all the way through)
    mov eax, [rbx+r15]
    jmp .qds_resolved

.qds_skip_rdata:
    add r15, r8
    dec rcx
    jmp .qds_answer_loop

.qds_resolved:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.qds_fail_close:
    mov rdi, r12
    mov rax, 3
    syscall
.qds_fail:
    mov rax, -1
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret
