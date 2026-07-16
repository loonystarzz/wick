; ============================================================
; den.asm - minimal wick "den" browser, pure x86-64 Linux asm
; No libc. Raw syscalls only. Renders local .wax files AND now
; fetches http:// URLs directly over a hand-rolled HTTP+DNS
; stack (same engine as the standalone httpget tool).
;
; Build:
;   nasm -f elf64 den.asm -o den.o
;   ld den.o -o den
;
; Usage:
;   ./den somefile.wax
;   ./den http://example.com/page.wax
;
; A link or starting page can be either a local file path (with
; /path/path2 style relative paths, unchanged from before) or a
; full "http://host/path" URL, which gets its scheme stripped,
; host resolved via DNS, and the response body rendered exactly
; like a local .wax file. https:// URLs are recognized as URLs
; too but there's no TLS here, so they'll fail to connect - this
; is a proof-of-concept HTTP stack, not a real browser network
; stack.
; ============================================================

section .data

usage_msg:      db "usage: den <file.wax | http://host/path>", 10
usage_len       equ $ - usage_msg

open_err_msg:   db "den: cannot open file", 10
open_err_len    equ $ - open_err_msg

net_err_msg:    db "den: network fetch failed", 10
net_err_len     equ $ - net_err_msg

prompt_msg:     db 10, "[1-9] link   q quit  > "
prompt_len      equ $ - prompt_msg

bold_on:        db 27, "[1m"
bold_on_len     equ $ - bold_on

reset_seq:      db 27, "[0m"
reset_len       equ $ - reset_seq

indent4:        db "    "
indent2:        db "  "
link_open:      db "["
link_close:     db "] "
nl_char:        db 10

; --- color escape sequences ($$x codes) ---
esc_black:      db 27,"[30m"
esc_black_len   equ $-esc_black
esc_darkgray:   db 27,"[90m"
esc_darkgray_len equ $-esc_darkgray
esc_darkblue:   db 27,"[34m"
esc_darkblue_len equ $-esc_darkblue
esc_blue:       db 27,"[94m"
esc_blue_len    equ $-esc_blue
esc_darkgreen:  db 27,"[32m"
esc_darkgreen_len equ $-esc_darkgreen
esc_green:      db 27,"[92m"
esc_green_len   equ $-esc_green
esc_darkcyan:   db 27,"[36m"
esc_darkcyan_len equ $-esc_darkcyan
esc_cyan:       db 27,"[96m"
esc_cyan_len    equ $-esc_cyan
esc_darkred:    db 27,"[31m"
esc_darkred_len equ $-esc_darkred
esc_red:        db 27,"[91m"
esc_red_len     equ $-esc_red
esc_darkmag:    db 27,"[35m"
esc_darkmag_len equ $-esc_darkmag
esc_magenta:    db 27,"[95m"
esc_magenta_len equ $-esc_magenta
esc_gold:       db 27,"[33m"
esc_gold_len    equ $-esc_gold
esc_yellow:     db 27,"[93m"
esc_yellow_len  equ $-esc_yellow
esc_gray:       db 27,"[37m"
esc_gray_len    equ $-esc_gray
esc_white:      db 27,"[97m"
esc_white_len   equ $-esc_white

; --- networking: URL scheme prefixes ---
http_prefix:      db "http://"
http_prefix_len   equ $ - http_prefix
https_prefix:     db "https://"
https_prefix_len  equ $ - https_prefix
default_path:     db "/", 0

; --- HTTP request pieces ---
req_get:        db "GET "
req_get_len     equ $ - req_get
req_httpver:    db " HTTP/1.1", 13, 10
req_httpver_len equ $ - req_httpver
req_host:       db "Host: "
req_host_len    equ $ - req_host
req_conn:       db 13, 10, "Connection: close", 13, 10, 13, 10
req_conn_len    equ $ - req_conn

; --- DNS ---
resolv_path:          db "/etc/resolv.conf", 0
nameserver_prefix:    db "nameserver "
nameserver_prefix_len equ $ - nameserver_prefix
fallback_dns:         db "8.8.8.8", 0
cloudflare_dns:       db "1.1.1.1", 0

dns_query_header:     db 0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
dns_query_header_len  equ $ - dns_query_header
dns_qtype_class:      db 0x00, 0x01, 0x00, 0x01
dns_qtype_class_len   equ $ - dns_qtype_class

section .bss

file_buf:       resb 65536
bytes_read:     resq 1
current_path:   resb 256
input_buf:      resb 128
digit_buf:      resb 1

num_links:      resq 1
links_len:      resq 9
links_url:      resb 9*256          ; 9 slots x 256 bytes each

; --- networking scratch ---
url_host:           resb 256
url_path:           resb 256
url_port_str:       resb 16

request_buf:        resb 1024
net_recv_buf:        resb 65536
net_recv_total:       resq 1
net_sockaddr:        resb 16          ; family(2) port(2) addr(4) pad(8)

resolv_buf:          resb 2048
resolver_ip_str:     resb 64
dns_query_buf:       resb 512
dns_resp_buf:        resb 2048
resolver_sockaddr:   resb 16

section .text
global _start

; ============================================================
; _start
; ============================================================
_start:
    mov rax, [rsp]              ; argc
    cmp rax, 2
    jl  .usage

    mov rsi, [rsp+16]           ; argv[1]
    lea rdi, [current_path]
    call strcpy

.main_loop:
    ; --- is current_path a URL? ---
    lea rdi, [current_path]
    call parse_url
    cmp rax, 1
    je  .fetch_network

    ; --- local file path: open/read/close as before ---
    lea rdi, [current_path]
    xor rsi, rsi                ; O_RDONLY
    xor rdx, rdx
    mov rax, 2                  ; sys_open
    syscall
    cmp rax, 0
    jl  .open_error
    mov r12, rax                ; fd

    mov rdi, r12
    lea rsi, [file_buf]
    mov rdx, 65535
    mov rax, 0                  ; sys_read
    syscall
    mov [bytes_read], rax

    mov rdi, r12
    mov rax, 3                  ; sys_close
    syscall
    jmp .have_content

.fetch_network:
    call fetch_over_http
    cmp rax, 0
    jl  .net_error
    ; fetch_over_http already populated file_buf + bytes_read

.have_content:
    ; --- reset link table ---
    mov qword [num_links], 0

    ; --- render ---
    call render

    ; --- prompt ---
    lea rsi, [prompt_msg]
    mov rdx, prompt_len
    mov rdi, 1
    mov rax, 1
    syscall

    ; --- read a line of input ---
    xor rdi, rdi
    lea rsi, [input_buf]
    mov rdx, 127
    mov rax, 0
    syscall
    cmp rax, 0
    jle .main_loop

    movzx eax, byte [input_buf]
    cmp al, 'q'
    je  .exit_ok
    cmp al, '1'
    jl  .main_loop
    cmp al, '9'
    jg  .main_loop

    ; digit selection -> index 0..8
    sub al, '1'
    movzx r13, al
    mov rax, [num_links]
    cmp r13, rax
    jge .main_loop               ; no such link, just redraw

    ; copy links_url[r13] (len links_len[r13]) into current_path
    mov rax, r13
    shl rax, 8                   ; *256
    lea rsi, [links_url]
    add rsi, rax
    mov rcx, [links_len + r13*8]
    lea rdi, [current_path]
    cld
    rep movsb
    mov byte [rdi], 0
    jmp .main_loop

.exit_ok:
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

.open_error:
    lea rsi, [open_err_msg]
    mov rdx, open_err_len
    mov rdi, 1
    mov rax, 1
    syscall
    mov rdi, 1
    mov rax, 60
    syscall

.net_error:
    lea rsi, [net_err_msg]
    mov rdx, net_err_len
    mov rdi, 1
    mov rax, 1
    syscall
    mov rdi, 1
    mov rax, 60
    syscall

; ============================================================
; strcpy: rdi=dest, rsi=src (null-terminated), max 255 bytes
; ============================================================
strcpy:
    xor rcx, rcx
.copy_loop:
    cmp rcx, 255
    jge .copy_done
    mov al, [rsi+rcx]
    mov [rdi+rcx], al
    cmp al, 0
    je  .copy_done
    inc rcx
    jmp .copy_loop
.copy_done:
    mov byte [rdi+rcx], 0
    ret

; ============================================================
; copy_str_null: rdi=dest, rsi=src (null-terminated) -> copies
; including the terminating null byte. rdi/rsi left past the end.
; ============================================================
copy_str_null:
    push rax
.csn_loop:
    mov al, [rsi]
    mov [rdi], al
    cmp al, 0
    je  .csn_done
    inc rsi
    inc rdi
    jmp .csn_loop
.csn_done:
    pop rax
    ret

; ============================================================
; strlen: rdi = pointer to null-terminated string -> rax = length
; ============================================================
strlen_:
    xor rax, rax
.sl_loop:
    cmp byte [rdi+rax], 0
    je  .sl_done
    inc rax
    jmp .sl_loop
.sl_done:
    ret

; ============================================================
; wr: rsi=ptr, rdx=len -> write to stdout (fd 1)
; ============================================================
wr:
    push rax
    push rdi
    push rcx
    push r11
    mov rdi, 1
    mov rax, 1
    syscall
    pop r11
    pop rcx
    pop rdi
    pop rax
    ret

; ============================================================
; newline: write a single 0x0A to stdout
; ============================================================
newline:
    push rax
    push rdi
    push rsi
    push rdx
    push rcx
    push r11
    lea rsi, [nl_char]
    mov rdx, 1
    mov rdi, 1
    mov rax, 1
    syscall
    pop r11
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rax
    ret

; ============================================================
; render: walks file_buf (length [bytes_read]) line by line
; ============================================================
render:
    lea r14, [file_buf]
    mov r15, [bytes_read]
    xor r8, r8
.line_loop:
    cmp r8, r15
    jge .render_done

    mov rdi, r14
    add rdi, r8                 ; line start
    mov rcx, r15
    sub rcx, r8                 ; remaining bytes

    xor r9, r9
.find_nl:
    cmp r9, rcx
    jge .no_nl
    mov al, [rdi+r9]
    cmp al, 10
    je  .found_nl
    inc r9
    jmp .find_nl
.found_nl:
    mov r10, r9                 ; line length excluding \n
    mov r11, r8
    add r11, r9
    inc r11                     ; skip the newline
    jmp .do_classify
.no_nl:
    mov r10, rcx
    mov r11, r15
.do_classify:
    call classify_and_print      ; rdi=line ptr, r10=len
    mov r8, r11
    jmp .line_loop
.render_done:
    ret

; ============================================================
; classify_and_print: rdi=line ptr, r10=line length
; Preserves r8, r11, r14, r15 across the call.
; ============================================================
classify_and_print:
    push r8
    push r11
    push r14
    push r15

    ; --- subheader: "##..." ---
    cmp r10, 2
    jl  .cp_not_sub
    cmp byte [rdi], '#'
    jne .cp_not_sub
    cmp byte [rdi+1], '#'
    jne .cp_not_sub

    lea rsi, [indent2]
    mov rdx, 2
    call wr
    lea rsi, [bold_on]
    mov rdx, bold_on_len
    call wr
    mov rsi, rdi
    add rsi, 2
    mov rdx, r10
    sub rdx, 2
    call wr
    lea rsi, [reset_seq]
    mov rdx, reset_len
    call wr
    call newline
    jmp .cp_exit

.cp_not_sub:
    ; --- header: "#..." ---
    cmp r10, 1
    jl  .cp_not_header
    cmp byte [rdi], '#'
    jne .cp_not_header

    lea rsi, [indent4]
    mov rdx, 4
    call wr
    lea rsi, [bold_on]
    mov rdx, bold_on_len
    call wr
    mov rsi, rdi
    inc rsi
    mov rdx, r10
    dec rdx
    call wr
    lea rsi, [reset_seq]
    mov rdx, reset_len
    call wr
    call newline
    jmp .cp_exit

.cp_not_header:
    ; --- link: "=> URL label" ---
    cmp r10, 3
    jl  .cp_not_link
    cmp byte [rdi], '='
    jne .cp_not_link
    cmp byte [rdi+1], '>'
    jne .cp_not_link
    cmp byte [rdi+2], ' '
    jne .cp_not_link

    mov rbx, rdi
    add rbx, 3
    mov rcx, r10
    sub rcx, 3

.cp_skip_sp1:
    cmp rcx, 0
    jle .cp_exit             ; malformed link line, skip silently
    cmp byte [rbx], ' '
    jne .cp_url_start
    inc rbx
    dec rcx
    jmp .cp_skip_sp1

.cp_url_start:
    mov r12, rbx              ; url start
    xor r13, r13               ; url length

.cp_url_scan:
    cmp r13, rcx
    jge .cp_url_end
    mov al, [rbx+r13]
    cmp al, ' '
    je  .cp_url_end
    inc r13
    jmp .cp_url_scan

.cp_url_end:
    mov rax, rbx
    add rax, r13                ; ptr just past url
    mov rdx, rcx
    sub rdx, r13                 ; remaining after url

.cp_skip_sp2:
    cmp rdx, 0
    jle .cp_have_label          ; no label text, rdx already 0
    cmp byte [rax], ' '
    jne .cp_have_label
    inc rax
    dec rdx
    jmp .cp_skip_sp2

.cp_have_label:
    ; rax = label ptr, rdx = label len
    push rax
    push rdx

    mov rsi, [num_links]
    cmp rsi, 9
    jge .cp_print_link           ; table full, skip storing this one

    mov rdi, rsi
    shl rdi, 8
    lea r8, [links_url]
    add r8, rdi

    push rsi
    mov rcx, r13
    mov rsi, r12
    mov rdi, r8
    cld
    rep movsb
    pop rsi

    mov [links_len + rsi*8], r13
    inc rsi
    mov [num_links], rsi

.cp_print_link:
    pop r13                       ; label len
    pop r12                       ; label ptr

    lea rsi, [link_open]
    mov rdx, 1
    call wr

    mov rax, [num_links]
    add al, '0'
    mov [digit_buf], al
    lea rsi, [digit_buf]
    mov rdx, 1
    call wr

    lea rsi, [link_close]
    mov rdx, 2
    call wr

    mov rsi, r12
    mov rdx, r13
    call wr
    call newline
    jmp .cp_exit

.cp_not_link:
    ; --- plain text, with $$x inline color codes ---
    xor rcx, rcx
.cp_plain_loop:
    cmp rcx, r10
    jge .cp_plain_done
    mov al, [rdi+rcx]
    cmp al, '$'
    jne .cp_plain_emit

    lea r11, [rcx+2]
    cmp r11, r10
    jg  .cp_plain_emit
    mov bl, [rdi+rcx+1]
    cmp bl, '$'
    jne .cp_plain_emit
    mov bl, [rdi+rcx+2]
    call get_color_escape
    cmp rdx, 0
    je  .cp_plain_emit

    call wr
    add rcx, 3
    jmp .cp_plain_loop

.cp_plain_emit:
    lea rsi, [rdi+rcx]
    mov rdx, 1
    call wr
    inc rcx
    jmp .cp_plain_loop

.cp_plain_done:
    lea rsi, [reset_seq]
    mov rdx, reset_len
    call wr
    call newline
    jmp .cp_exit

.cp_exit:
    pop r15
    pop r14
    pop r11
    pop r8
    ret

; ============================================================
; get_color_escape: bl=color char -> rsi=ptr, rdx=len (rdx=0 if
; the char isn't a recognized color code)
; ============================================================
get_color_escape:
    cmp bl, '0'
    je .g0
    cmp bl, '8'
    je .g8
    cmp bl, '1'
    je .g1
    cmp bl, '9'
    je .g9
    cmp bl, '2'
    je .g2
    cmp bl, 'a'
    je .ga
    cmp bl, '3'
    je .g3
    cmp bl, 'b'
    je .gb
    cmp bl, '4'
    je .g4
    cmp bl, 'c'
    je .gc
    cmp bl, '5'
    je .g5
    cmp bl, 'd'
    je .gd
    cmp bl, '6'
    je .g6
    cmp bl, 'e'
    je .ge
    cmp bl, '7'
    je .g7
    cmp bl, 'f'
    je .gf
    cmp bl, 'r'
    je .gr
    xor rdx, rdx
    ret
.g0:
    lea rsi, [esc_black]
    mov rdx, esc_black_len
    ret
.g8:
    lea rsi, [esc_darkgray]
    mov rdx, esc_darkgray_len
    ret
.g1:
    lea rsi, [esc_darkblue]
    mov rdx, esc_darkblue_len
    ret
.g9:
    lea rsi, [esc_blue]
    mov rdx, esc_blue_len
    ret
.g2:
    lea rsi, [esc_darkgreen]
    mov rdx, esc_darkgreen_len
    ret
.ga:
    lea rsi, [esc_green]
    mov rdx, esc_green_len
    ret
.g3:
    lea rsi, [esc_darkcyan]
    mov rdx, esc_darkcyan_len
    ret
.gb:
    lea rsi, [esc_cyan]
    mov rdx, esc_cyan_len
    ret
.g4:
    lea rsi, [esc_darkred]
    mov rdx, esc_darkred_len
    ret
.gc:
    lea rsi, [esc_red]
    mov rdx, esc_red_len
    ret
.g5:
    lea rsi, [esc_darkmag]
    mov rdx, esc_darkmag_len
    ret
.gd:
    lea rsi, [esc_magenta]
    mov rdx, esc_magenta_len
    ret
.g6:
    lea rsi, [esc_gold]
    mov rdx, esc_gold_len
    ret
.ge:
    lea rsi, [esc_yellow]
    mov rdx, esc_yellow_len
    ret
.g7:
    lea rsi, [esc_gray]
    mov rdx, esc_gray_len
    ret
.gf:
    lea rsi, [esc_white]
    mov rdx, esc_white_len
    ret
.gr:
    lea rsi, [reset_seq]
    mov rdx, reset_len
    ret

; ============================================================
; ================ NETWORKING (HTTP + DNS) ==================
; ============================================================

; ============================================================
; parse_url: rdi = string to check
; If it starts with "http://" or "https://", strips the scheme,
; splits the rest into url_host / url_path / url_port_str (all
; null-terminated, url_path defaults to "/" if none given, e.g.
; "http://blahblah.com/" -> host="blahblah.com" path="/", and
; "http://blahblah.com/path/path2" -> host="blahblah.com"
; path="/path/path2").
; returns: rax = 1 if it was a URL (fields populated), else 0
;          (fields left untouched, caller treats as local path)
; ============================================================
parse_url:
    call strlen_                  ; rdi unchanged, rax = length
    mov r9, rax                   ; r9 = total length of input

    cmp r9, http_prefix_len
    jl  .pu_try_https
    push rdi
    lea rsi, [http_prefix]
    mov rdx, http_prefix_len
    call mem_compare
    pop rdi
    cmp rax, 1
    je  .pu_has_http

.pu_try_https:
    cmp r9, https_prefix_len
    jl  .pu_not_url
    push rdi
    lea rsi, [https_prefix]
    mov rdx, https_prefix_len
    call mem_compare
    pop rdi
    cmp rax, 1
    je  .pu_has_https
    jmp .pu_not_url

.pu_has_http:
    add rdi, http_prefix_len
    jmp .pu_parse_host
.pu_has_https:
    add rdi, https_prefix_len

.pu_parse_host:
    mov rsi, rdi                  ; rsi = cursor into host/path/port
    lea rdi, [url_host]
    xor rcx, rcx
.pu_host_loop:
    mov al, [rsi]
    cmp al, 0
    je  .pu_host_end
    cmp al, '/'
    je  .pu_host_end
    cmp al, ':'
    je  .pu_host_end_colon
    mov [rdi+rcx], al
    inc rcx
    inc rsi
    jmp .pu_host_loop

.pu_host_end_colon:
    mov byte [rdi+rcx], 0
    inc rsi                        ; skip ':'
    lea rdi, [url_port_str]
    xor rdx, rdx
.pu_port_loop:
    mov al, [rsi]
    cmp al, 0
    je  .pu_port_done
    cmp al, '/'
    je  .pu_port_done
    mov [rdi+rdx], al
    inc rdx
    inc rsi
    jmp .pu_port_loop
.pu_port_done:
    mov byte [rdi+rdx], 0
    jmp .pu_finish_path

.pu_host_end:
    mov byte [rdi+rcx], 0
    mov byte [url_port_str], 0
    jmp .pu_finish_path

.pu_finish_path:
    ; rsi now points at '/' or the terminating NUL - the rest
    ; (if any) is the path, kept fully intact (/path/path2 etc.)
    mov al, [rsi]
    cmp al, 0
    jne .pu_copy_real_path
    lea rsi, [default_path]        ; no path given -> "/"
.pu_copy_real_path:
    lea rdi, [url_path]
    call copy_str_null
    mov rax, 1
    ret

.pu_not_url:
    xor rax, rax
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
; Tries the system resolver (from /etc/resolv.conf, or 8.8.8.8
; if that file is missing/empty) first, and ONLY if that attempt
; fails does it retry once against Cloudflare's 1.1.1.1.
; ============================================================
resolve_dns:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r12, rdi                  ; keep hostname safe across calls

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

    xor rcx, rcx
.scan_loop:
    mov rax, r14
    sub rax, rcx
    cmp rax, nameserver_prefix_len
    jl  .use_fallback_dns

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
    call copy_str_null

.have_resolver_ip:
    ; --- attempt 1: system resolver (or 8.8.8.8 fallback) ---
    lea rsi, [resolver_ip_str]
    mov rdi, r12
    call query_dns_server
    cmp rax, 0
    jge .resolve_done

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
; query_dns_server: rdi = hostname, rsi = resolver IPv4 (string)
; Sends one DNS query to that server and parses the first A
; record out of the reply.
; returns: rax = resolved addr (network byte order), or -1
; ============================================================
query_dns_server:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r12, rdi                   ; hostname

    mov rdi, rsi                   ; resolver ip string
    call parse_ipv4
    cmp rax, 0
    jl  .qds_fail
    mov r9d, eax

    lea rbx, [resolver_sockaddr]
    mov word [rbx], 2
    mov word [rbx+2], 0x3500       ; port 53, network order
    mov [rbx+4], r9d
    mov qword [rbx+8], 0

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
    sub r13, rax                   ; total query length

    mov rdi, 2                     ; AF_INET
    mov rsi, 2                     ; SOCK_DGRAM
    xor rdx, rdx
    mov rax, 41                    ; socket
    syscall
    cmp rax, 0
    jl  .qds_fail
    mov r12, rax                    ; udp fd (r11 is NOT safe here -
                                     ; `syscall` clobbers rcx/r11)

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

    lea rbx, [dns_resp_buf]

    movzx rax, byte [rbx+6]
    shl rax, 8
    movzx rdx, byte [rbx+7]
    or  rax, rdx
    mov r14, rax                   ; ANCOUNT
    cmp r14, 0
    jle .qds_fail

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
    add r15, 4

    mov rcx, r14
.qds_answer_loop:
    cmp rcx, 0
    jle .qds_fail

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

    movzx rax, byte [rbx+r15]
    shl rax, 8
    movzx rdx, byte [rbx+r15+1]
    or  rax, rdx
    push rax
    add r15, 2

    add r15, 2                      ; CLASS
    add r15, 4                      ; TTL

    movzx rax, byte [rbx+r15]
    shl rax, 8
    movzx rdx, byte [rbx+r15+1]
    or  rax, rdx
    mov r8, rax                      ; rdlength
    add r15, 2

    pop rdx
    cmp rdx, 1
    jne .qds_skip_rdata
    cmp r8, 4
    jne .qds_skip_rdata

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
; find_body: locate the blank line ("\r\n\r\n" or "\n\n") that
; separates HTTP headers from the body.
; rdi = buffer, rsi = length
; returns: rax = offset of the first body byte, or -1 if no
;          blank-line separator was found (whole buffer is used
;          as a fallback body by the caller in that case)
; ============================================================
find_body:
    xor rcx, rcx
.fb_loop:
    mov rax, rsi
    sub rax, rcx
    cmp rax, 4
    jl  .fb_try_lf_lf
    cmp byte [rdi+rcx], 13
    jne .fb_next
    cmp byte [rdi+rcx+1], 10
    jne .fb_next
    cmp byte [rdi+rcx+2], 13
    jne .fb_next
    cmp byte [rdi+rcx+3], 10
    jne .fb_next
    mov rax, rcx
    add rax, 4
    ret
.fb_next:
    inc rcx
    cmp rcx, rsi
    jl  .fb_loop
    jmp .fb_not_found

.fb_try_lf_lf:
    xor rcx, rcx
.fb_loop2:
    mov rax, rsi
    sub rax, rcx
    cmp rax, 2
    jl  .fb_not_found
    cmp byte [rdi+rcx], 10
    jne .fb_next2
    cmp byte [rdi+rcx+1], 10
    jne .fb_next2
    mov rax, rcx
    add rax, 2
    ret
.fb_next2:
    inc rcx
    cmp rcx, rsi
    jl  .fb_loop2
.fb_not_found:
    mov rax, -1
    ret

; ============================================================
; fetch_over_http: reads url_host/url_path/url_port_str (already
; populated by parse_url), performs DNS resolution if needed,
; connects, sends a GET request, reads the whole response, and
; copies the body into file_buf / sets bytes_read.
; returns: rax = 0 on success, -1 on any failure
; ============================================================
fetch_over_http:
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; --- resolve host: literal dotted IPv4 first, else DNS ---
    lea rdi, [url_host]
    call parse_ipv4
    cmp rax, 0
    jl  .fh_try_dns
    mov r8, rax
    jmp .fh_have_addr
.fh_try_dns:
    lea rdi, [url_host]
    call resolve_dns
    cmp rax, 0
    jl  .fh_fail
    mov r8, rax
.fh_have_addr:

    ; --- determine port: url_port_str empty -> 80 ---
    mov r15w, 80
    cmp byte [url_port_str], 0
    je  .fh_no_port
    lea rdi, [url_port_str]
    call parse_uint16
    mov r15w, ax
.fh_no_port:
    mov ax, r15w
    xchg al, ah
    mov r15w, ax                   ; port, network byte order now

    ; --- build sockaddr_in ---
    lea rbx, [net_sockaddr]
    mov word [rbx], 2              ; AF_INET
    mov [rbx+2], r15w
    mov [rbx+4], r8d
    mov qword [rbx+8], 0

    ; --- socket + connect ---
    mov rdi, 2
    mov rsi, 1
    xor rdx, rdx
    mov rax, 41                    ; socket
    syscall
    cmp rax, 0
    jl  .fh_fail
    mov r12, rax                   ; tcp fd

    mov rdi, r12
    lea rsi, [net_sockaddr]
    mov rdx, 16
    mov rax, 42                    ; connect
    syscall
    cmp rax, 0
    jl  .fh_fail_close

    ; --- build the GET request ---
    lea rdi, [request_buf]

    lea rsi, [req_get]
    mov rcx, req_get_len
    call membuf_copy

    lea rsi, [url_path]
    call strlen_copy_

    lea rsi, [req_httpver]
    mov rcx, req_httpver_len
    call membuf_copy

    lea rsi, [req_host]
    mov rcx, req_host_len
    call membuf_copy

    lea rsi, [url_host]
    call strlen_copy_

    lea rsi, [req_conn]
    mov rcx, req_conn_len
    call membuf_copy

    lea rax, [request_buf]
    mov r13, rdi
    sub r13, rax                   ; request length

    ; --- write the request ---
    mov rdi, r12
    lea rsi, [request_buf]
    mov rdx, r13
    mov rax, 1                     ; write
    syscall

    ; --- read the entire response into net_recv_buf ---
    mov qword [net_recv_total], 0
.fh_read_loop:
    mov rax, [net_recv_total]
    cmp rax, 65535
    jge .fh_read_done              ; buffer full, stop

    mov rdi, r12
    lea rsi, [net_recv_buf]
    add rsi, rax                   ; append past what we already have
    mov rdx, 65535
    sub rdx, rax
    mov rax, 0                     ; read
    syscall
    cmp rax, 0
    jle .fh_read_done              ; 0=EOF, <0=error, either way stop

    add [net_recv_total], rax
    jmp .fh_read_loop

.fh_read_done:
    mov rdi, r12
    mov rax, 3                     ; close
    syscall

    mov r14, [net_recv_total]
    cmp r14, 0
    jle .fh_fail                   ; nothing came back at all

    ; --- split headers from body ---
    lea rdi, [net_recv_buf]
    mov rsi, r14
    call find_body
    cmp rax, 0
    jl  .fh_no_header_split         ; no blank line found - use whole buffer

    mov r15, rax                    ; body offset
    jmp .fh_copy_body

.fh_no_header_split:
    xor r15, r15                    ; treat entire response as body

.fh_copy_body:
    mov rcx, r14
    sub rcx, r15                    ; body length
    cmp rcx, 65535
    jle .fh_len_ok
    mov rcx, 65535
.fh_len_ok:
    mov [bytes_read], rcx

    lea rsi, [net_recv_buf]
    add rsi, r15
    lea rdi, [file_buf]
    call membuf_copy

    xor rax, rax                    ; success
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.fh_fail_close:
    mov rdi, r12
    mov rax, 3
    syscall
.fh_fail:
    mov rax, -1
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; ============================================================
; strlen_copy_: rdi=dest, rsi=src (null-terminated) -> copies,
; advances rdi past the copied bytes (no null terminator written)
; ============================================================
strlen_copy_:
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
