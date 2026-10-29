"""Minimal SMTP server for testing the TaskFlow mailer (plain, STARTTLS and implicit TLS).
   python3 fake_smtp.py PORT MODE(plain|starttls|ssl) CERT KEY OUTFILE"""
import socket, ssl, sys, threading, base64

port, mode, cert, key, out = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)


def handle(conn):
    f = conn
    tls = mode == "ssl"
    if tls:
        f = ctx.wrap_socket(conn, server_side=True)
    buf = b""

    def send(s):
        f.sendall((s + "\r\n").encode())

    def line():
        nonlocal buf
        while b"\r\n" not in buf:
            d = f.recv(4096)
            if not d:
                return None
            buf += d
        l, buf = buf.split(b"\r\n", 1)
        return l.decode("utf-8", "replace")

    send("220 fake ESMTP")
    authed = False
    mail = {"to": None, "data": []}
    while True:
        l = line()
        if l is None:
            return
        u = l.upper()
        if u.startswith("EHLO"):
            caps = ["250-fake"]
            if mode == "starttls" and not tls:
                caps.append("250-STARTTLS")
            caps.append("250 AUTH LOGIN PLAIN")
            for c in caps:
                send(c)
        elif u == "STARTTLS":
            send("220 go ahead")
            f = ctx.wrap_socket(conn, server_side=True)
            tls = True
            buf = b""
        elif u.startswith("AUTH PLAIN"):
            parts = base64.b64decode(l.split()[2]).split(b"\0")
            authed = parts[1] == b"mailer@test" and parts[2] == b"s3cret"
            send("235 ok" if authed else "535 bad")
        elif u.startswith("MAIL FROM"):
            send("250 ok" if (authed and (tls or mode == "plain")) else "530 auth first")
        elif u.startswith("RCPT TO"):
            mail["to"] = l[8:]
            send("250 ok")
        elif u == "DATA":
            send("354 go")
            while True:
                d = line()
                if d == ".":
                    break
                mail["data"].append(d)
            with open(out, "a") as o:
                o.write("TO " + mail["to"] + "\n" + "\n".join(mail["data"]) + "\n=====\n")
            send("250 queued")
        elif u == "QUIT":
            send("221 bye")
            return
        else:
            send("500 ?")


s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(5)
while True:
    c, _ = s.accept()
    threading.Thread(target=lambda: (handle(c), c.close()), daemon=True).start()
