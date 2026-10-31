"""End-to-end test of the Inverted TaskFlow server API.

    python3 tests/api_test.py path/to/TaskFlowServer

Starts its own server on a free port with a fresh demo database, runs every check, stops the server.
"""
import json, os, shutil, socket, subprocess, sys, tempfile, threading, time, urllib.error, urllib.request

EXE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/server/TaskFlowServer")
PASS = FAIL = 0
FAILS = []


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


PORT = free_port()
BASE = f"http://127.0.0.1:{PORT}/api/v1"
WORK = tempfile.mkdtemp(prefix="tf_api_")
with open(os.path.join(WORK, "server.ini"), "w") as f:
    f.write(f"port = {PORT}\nbind = 127.0.0.1\nadmin_email = admin@taskflow.local\nadmin_password = Admin@123\n"
            "backup_dir = backups\nlog_file = logs/server.log\nrate_limit_per_ip = 300\n")
CONSOLE = open(os.path.join(WORK, "console.txt"), "w")
CMD = [EXE]
if EXE.lower().endswith(".exe") and os.name != "nt":  # Windows build tested under Wine
    CMD = [os.environ.get("WINE", "/usr/lib/wine/wine64"), EXE]
proc = subprocess.Popen(CMD + ["--config", os.path.join(WORK, "server.ini"), "--demo"], cwd=WORK,
                        stdout=CONSOLE, stderr=subprocess.STDOUT)
for _ in range(300):
    try:
        urllib.request.urlopen(BASE + "/health", timeout=1)
        break
    except Exception:
        time.sleep(0.1)


def call(method, path, body=None, token=None, raw=None, timeout=40):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(BASE + path, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        txt = e.read()
        try:
            return e.code, json.loads(txt or b"{}")
        except Exception:
            return e.code, {"raw": txt[:200]}


def check(name, cond, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  ok   ", name)
    else:
        FAIL += 1
        FAILS.append(name)
        print("  FAIL ", name, str(extra)[:400])


def login(email, pw="Demo@123"):
    s, r = call("POST", "/auth/login", {"email": email, "password": pw})
    assert s == 200, (email, s, r)
    return r["token"], r["user"]


def sync(tok, cursor=0, scope=0, wait=0):
    s, r = call("GET", f"/sync?cursor={cursor}&scope={scope}&wait={wait}", token=tok)
    assert s == 200, r
    return r


def find(rows, **kw):
    return [x for x in rows if all(x.get(k) == v for k, v in kw.items())]


def task_by_title(tok, title):
    r = find(sync(tok)["tasks"], title=title)
    return r[0] if r else None


def notifs(tok, cursor=0):
    return sync(tok, cursor).get("notifications", [])


try:
    # ------------------------------------------------------------------ auth & security
    print("== sign-in and accounts")
    s, r = call("GET", "/company")
    check("company info is public", s == 200 and r["name"].startswith("Inverted"), r)
    s, r = call("GET", "/departments/public")
    names = {d["name"] for d in r["departments"]}
    check("11 standard departments exist", {"Management", "Electronics Engineering", "Mechanical Engineering", "Quality",
                                            "Process", "Production", "Sales", "Supply Chain Management", "Service", "IT",
                                            "HR"} <= names, names)
    s, r = call("POST", "/auth/login", {"email": "admin@taskflow.local", "password": "wrong"})
    check("wrong password rejected", s == 401)
    s, r = call("POST", "/auth/login", {"email": "' OR 1=1 --", "password": "x' OR '1'='1"})
    check("SQL injection in sign-in rejected", s == 401, r)
    admin, _ = login("admin@taskflow.local", "Admin@123")
    s, r = call("POST", "/auth/register", {"name": "Test User", "email": "test@demo.local", "password": "short"})
    check("weak password rejected", s == 400, r)
    s, r = call("POST", "/auth/register", {"name": "Test User", "email": "not-an-email", "password": "Testing123"})
    check("invalid e-mail rejected", s == 400, r)
    s, deps = call("GET", "/departments/public")
    PRD = find(deps["departments"], code="PRD")[0]["id"]
    L2 = find(deps["departments"], code="PRDL2")[0]["id"]
    L1 = find(deps["departments"], code="PRDL1")[0]["id"]
    QUA = find(deps["departments"], code="QUA")[0]["id"]
    IQC = find(deps["departments"], code="QUAIQ")[0]["id"]
    MEC = find(deps["departments"], code="MEC")[0]["id"]
    SCM = find(deps["departments"], code="SCM")[0]["id"]
    check("sub-departments are listed with their parent", find(deps["departments"], code="PRDL2")[0]["parent_id"] == PRD)
    s, r = call("POST", "/auth/register", {"name": "Test User", "email": "test@demo.local", "password": "Testing123", "dept_id": L2})
    check("register creates a pending account", s == 200, r)
    s, r = call("POST", "/auth/register", {"name": "Again", "email": "TEST@demo.local", "password": "Testing123"})
    check("duplicate e-mail (any case) rejected", s == 409, r)
    s, r = call("POST", "/auth/login", {"email": "test@demo.local", "password": "Testing123"})
    check("pending account cannot sign in", s == 403 and "approval" in r.get("error", ""), r)
    s, users = call("GET", "/admin/users", token=admin)
    test_id = find(users["users"], email="test@demo.local")[0]["id"]
    vik, vik_u = login("vikram@demo.local")
    s, r = call("GET", "/admin/users", token=vik)
    check("non-admin blocked from admin API", s == 403)
    s, r = call("POST", f"/admin/users/{test_id}/approve", {"global_role": "STAFF", "dept_id": L2, "dept_role": "MEMBER"}, admin)
    check("admin approves account into a sub-department", s == 200, r)
    test_tok, test_user = login("test@demo.local", "Testing123")
    check("approved user signs in with unit", test_user["units"][0]["code"] == "PRDL2", test_user)
    s, r = call("GET", "/me", token="0" * 64)
    check("bad token rejected with SESSION_EXPIRED", s == 401 and r.get("code") == "SESSION_EXPIRED", r)

    # lockout
    for i in range(5):
        call("POST", "/auth/login", {"email": "test@demo.local", "password": "Wrong12345"})
    s, r = call("POST", "/auth/login", {"email": "test@demo.local", "password": "Testing123"})
    check("account locked after 5 wrong passwords", s == 423, r)

    # must change password
    s, r = call("POST", f"/admin/users/{test_id}/reset-password", {"password": "Temp12345"}, admin)
    check("admin resets password (also unlocks)", s == 200, r)
    tmp_tok, tmp_u = login("test@demo.local", "Temp12345")
    check("reset account flagged must_change_password", tmp_u["must_change_password"] == 1, tmp_u)
    s, r = call("GET", "/sync?cursor=0", token=tmp_tok)
    check("API blocked until password changed", s == 403 and r.get("code") == "PASSWORD_CHANGE_REQUIRED", r)
    s, r = call("POST", "/auth/password", {"old_password": "Temp12345", "new_password": "Temp12345"}, tmp_tok)
    check("same password refused", s == 400, r)
    s, r = call("POST", "/auth/password", {"old_password": "Temp12345", "new_password": "NewPass2025"}, tmp_tok)
    check("password changed", s == 200, r)
    s, r = call("GET", "/sync?cursor=0", token=tmp_tok)
    check("API works after password change", s == 200, r)

    s, r = call("POST", "/tasks", raw=b"{not json", token=vik)
    check("invalid JSON rejected", s == 400, r)
    s, r = call("POST", "/tasks/1/comments", {"body": "x" * (1100 * 1024)}, vik)
    check("oversized payload rejected", s == 413, r)

    # ------------------------------------------------------------------ visibility (two-level organisation)
    print("== who sees what")
    rahul, rahul_u = login("rahul@demo.local")
    sneha, sneha_u = login("sneha@demo.local")
    manoj, manoj_u = login("manoj@demo.local")
    imran, imran_u = login("imran@demo.local")
    pooja, pooja_u = login("pooja@demo.local")
    suresh, suresh_u = login("suresh@demo.local")
    anita, anita_u = login("director@demo.local")
    sameer, _ = login("cfo@demo.local")
    neha, neha_u = login("neha@demo.local")
    amit, amit_u = login("amit@demo.local")
    lakshmi, lakshmi_u = login("lakshmi@demo.local")
    priya, priya_u = login("priya@demo.local")
    kavya, kavya_u = login("kavya@demo.local")
    farhan, farhan_u = login("farhan@demo.local")
    arjun, arjun_u = login("arjun@demo.local")
    rohan, rohan_u = login("rohan@demo.local")
    nikhil, nikhil_u = login("nikhil@demo.local")

    total = len(sync(admin)["tasks"])
    sr = sync(rahul)
    rk = {t["title"] for t in sr["tasks"]}
    check("sub-dept head sees his unit's tasks", "Operator training - shift B" in rk and "Cycle-time study for module stacking" in rk, rk)
    check("sub-dept head does not see the other line", "Daily 5S audit - this week" not in rk, rk)
    check("sub-dept head does not see other departments", not any(t["key"].startswith("GEN3") for t in sr["tasks"]))
    sv = sync(vik)
    vk = {t["title"] for t in sv["tasks"]}
    check("department head sees all sub-departments", "Daily 5S audit - this week" in vk and "Operator training - shift B" in vk, vk)
    si = sync(imran)
    check("deputy sees the same as the head", {t["id"] for t in si["tasks"]} == {t["id"] for t in sv["tasks"]})
    ss = sync(sneha)
    sk = {t["title"] for t in ss["tasks"]}
    check("member sees own tasks", "Weld parameter trials (30 samples)" in sk and "Update PFMEA for the new welding cell" in sk, sk)
    check("member sees the main task of her sub-task", "Install second laser welding station" in sk, sk)
    check("member sees sibling sub-tasks", "Operator SOP for station 2" in sk, sk)
    check("a project member sees colleagues' tasks in the same project", "Operator training - shift B" in sk, sk)
    check("... but not other teams' routine work", "Daily 5S audit - this week" not in sk, sk)
    check("member does not see colleague's proposal", "Poka-yoke fixture for cell polarity check" not in sk, sk)
    sa = sync(anita)
    check("management head sees every task", len(sa["tasks"]) == total, (len(sa["tasks"]), total))
    check("management member sees every task", len(sync(sameer)["tasks"]) == total)
    check("member cannot read a hidden task", call("GET", f"/tasks/{find(sv['tasks'], title='Daily 5S audit - this week')[0]['id']}",
                                                    token=sneha)[0] == 404)

    # ------------------------------------------------------------------ completion review flow
    print("== mark complete -> review -> approve / reject / withdraw")
    weld = task_by_title(rahul, "Install second laser welding station")
    trials = task_by_title(sneha, "Weld parameter trials (30 samples)")
    check("main task progress comes from sub-tasks (2 of 4 = 50%)", weld["progress"] == 50, weld)
    cur = sync(rahul)["cursor"]
    s, r = call("POST", f"/tasks/{trials['id']}/submit", {"note": ""}, sneha)
    check("completion needs a comment", s == 400, r)
    s, r = call("POST", f"/tasks/{trials['id']}/submit", {"note": "done", "link": "javascript:alert(1)"}, sneha)
    check("report link must be http(s)", s == 400, r)
    s, r = call("POST", f"/tasks/{trials['id']}/submit", {"note": "30 samples pulled, all above 1.2 kN",
                                                          "link": "https://intranet/reports/weld.pdf"}, manoj)
    check("only the assignee can mark complete", s == 403, r)
    s, r = call("POST", f"/tasks/{trials['id']}/submit", {"note": "30 samples pulled, all above 1.2 kN",
                                                          "link": "https://intranet/reports/weld.pdf"}, sneha)
    check("assignee marks complete -> IN_REVIEW", s == 200 and r["status"] == "IN_REVIEW" and r["reviewers"] == ["Rahul Verma"], r)
    n = [x for x in notifs(rahul, cur) if x["type"] == "REVIEW_REQUESTED" and x["task_id"] == trials["id"]]
    check("direct manager (sub-dept head) notified to review", len(n) == 1, n)
    check("department head is not asked to review a member's task",
          not [x for x in notifs(vik) if x["type"] == "REVIEW_REQUESTED" and x["task_id"] == trials["id"]])
    t = task_by_title(sneha, "Weld parameter trials (30 samples)")
    check("task shows In review (not done) until approved", t["status"] == "IN_REVIEW")
    s, r = call("POST", f"/tasks/{trials['id']}/approve", {}, manoj)
    check("a colleague cannot approve", s == 403, r)
    s, r = call("POST", f"/tasks/{trials['id']}/approve", {}, sneha)
    check("the assignee cannot approve herself", s == 403, r)
    cur = sync(rahul)["cursor"]
    s, r = call("POST", f"/tasks/{trials['id']}/withdraw", {}, sneha)
    check("member withdraws the request before approval", s == 200 and task_by_title(sneha, trials["title"])["status"] == "IN_PROGRESS", r)
    check("reviewer told about the withdrawal", [x for x in notifs(rahul, cur) if x["type"] == "REVIEW_WITHDRAWN"])
    s, r = call("POST", f"/tasks/{trials['id']}/approve", {}, rahul)
    check("cannot approve a withdrawn request", s == 400, r)
    call("POST", f"/tasks/{trials['id']}/submit", {"note": "Re-submitted with shift B data"}, sneha)
    s, r = call("POST", f"/tasks/{trials['id']}/reject", {"note": ""}, rahul)
    check("rejection needs comments", s == 400, r)
    cur = sync(sneha)["cursor"]
    s, r = call("POST", f"/tasks/{trials['id']}/reject", {"note": "Please add the cross-section photos"}, rahul)
    check("manager rejects -> back to In progress", s == 200 and r["status"] == "IN_PROGRESS", r)
    n = [x for x in notifs(sneha, cur) if x["type"] == "TASK_REJECTED"]
    check("member sees the manager's rejection comments", n and "cross-section photos" in n[0]["body"], n)
    d = call("GET", f"/tasks/{trials['id']}", token=sneha)[1]
    comp = d["completions"]
    check("review history kept (withdrawn, rejected)", [c["status"] for c in comp] == ["WITHDRAWN", "REJECTED"], comp)
    call("POST", f"/tasks/{trials['id']}/submit", {"note": "Photos added", "link": "http://nas/weld"}, sneha)
    s, r = call("POST", f"/tasks/{trials['id']}/approve", {"note": "Good work"}, rahul)
    check("manager approves -> DONE", s == 200 and r["status"] == "DONE", r)
    weld = task_by_title(rahul, weld["title"])
    check("main task progress moved 50% -> 75%", weld["progress"] == 75, weld)

    print("== two-level management")
    dash = task_by_title(rahul, "Line 2 daily output dashboard")
    s, r = call("POST", f"/tasks/{dash['id']}/approve", {}, rahul)
    check("sub-dept head cannot approve his own task", s == 403, r)
    check("department head was asked to review the sub-dept head's task",
          [x for x in notifs(vik) if x["type"] == "REVIEW_REQUESTED" and x["task_id"] == dash["id"]])
    s, r = call("POST", f"/tasks/{dash['id']}/approve", {"note": "Great"}, vik)
    check("department head approves the sub-dept head's task", s == 200 and r["status"] == "DONE", r)
    l1 = task_by_title(vik, "Daily 5S audit - this week")
    s, r = call("POST", f"/tasks/{l1['id']}/start", {}, rahul)
    check("sub-dept head has no authority over another sub-department", s in (403, 404), r)
    cyc = task_by_title(rahul, "Cycle-time study for module stacking")
    s, r = call("POST", f"/tasks/{cyc['id']}/reject", {"note": "Need the jig proposal too"}, vik)
    check("department head may also decide a member's review", s == 200, r)

    print("== sub-tasks and automatic progress")
    s, r = call("POST", "/tasks", {"parent_id": weld["id"], "title": "Safety sign-off", "assignee_id": manoj_u["id"],
                                   "due_date": weld["due_date"]}, rahul)
    check("manager adds a sub-task for a team member", s == 200, r)
    safety = r.get("id")
    weld = task_by_title(rahul, weld["title"])
    check("progress recalculated with the new sub-task (3/5 = 60%)", weld["progress"] == 60, weld)
    check("new sub-task notified to the assignee",
          [x for x in notifs(manoj) if x["type"] == "SUBTASK_ASSIGNED" and x["task_id"] == safety])
    s, r = call("POST", "/tasks", {"parent_id": weld["id"], "title": "x", "assignee_id": sneha_u["id"], "due_date": weld["due_date"]}, sneha)
    check("member cannot add sub-tasks to the manager's task", s == 403, r)
    s, r = call("POST", "/tasks", {"parent_id": safety, "title": "x", "due_date": weld["due_date"]}, rahul)
    check("no sub-tasks of sub-tasks", s == 400, r)
    s, r = call("POST", "/tasks", {"parent_id": weld["id"], "title": "late", "assignee_id": manoj_u["id"], "due_date": "2099-01-01"}, rahul)
    check("sub-task cannot be due after its main task", s == 400, r)
    pf = task_by_title(sneha, "Update PFMEA for the new welding cell")
    s, r = call("POST", "/tasks", {"parent_id": pf["id"], "title": "Collect weld trial results", "due_date": pf["due_date"]}, sneha)
    check("member breaks down her own task into sub-tasks", s == 200, r)
    own_sub = r["id"]
    s, r = call("POST", f"/tasks/{pf['id']}/submit", {"note": "done"}, sneha)
    check("main task cannot be completed while sub-tasks are open", s == 400 and "sub-task" in r["error"], r)
    s, r = call("POST", f"/tasks/{own_sub}/submit", {}, sneha)
    check("own sub-task closes without review or comment", s == 200 and r["status"] == "DONE", r)
    check("progress of own task now 100% of sub-tasks", task_by_title(sneha, pf["title"])["progress"] == 100)
    s, r = call("POST", f"/tasks/{safety}/cancel", {"comment": "Covered by the commissioning checklist"}, rahul)
    check("manager cancels a sub-task", s == 200, r)
    check("cancelled sub-tasks are ignored in progress (3/4 = 75%)", task_by_title(rahul, weld["title"])["progress"] == 75)

    print("== cross-team work and the two-level chain")
    enc = task_by_title(kavya, "Enclosure design freeze (rev C)")
    rel = task_by_title(kavya, "Drawing release to PDM")
    call("POST", f"/tasks/{rel['id']}/submit", {}, kavya)
    s, r = call("POST", f"/tasks/{enc['id']}/submit", {"note": "Rev C frozen", "link": "https://pdm/encl-revC"}, kavya)
    check("cross-team task reviewed by the doing team's manager", s == 200 and r["reviewers"] == ["Farhan Ali"], r)
    cur = sync(arjun)["cursor"]
    call("POST", f"/tasks/{enc['id']}/approve", {}, farhan)
    check("requester told when the cross-team task is done",
          [x for x in notifs(arjun, cur) if x["type"] == "TASK_COMPLETED" and x["task_id"] == enc["id"]])
    s, r = call("POST", "/tasks", {"project_id": task_by_title(anita, "Line balancing simulation")["project_id"],
                                   "title": "Board pack for November", "due_date": "2030-01-01"}, anita)
    check("management head adds own task", s == 200, r)
    s, r = call("POST", f"/tasks/{r['id']}/submit", {"note": "Sent"}, anita)
    check("top of the company: completion closes automatically", s == 200 and r["status"] == "DONE", r)

    print("== requests: proposals, help, cross-team, extensions")
    sr = sync(rahul)
    closed = find(sr["requests"], type="TEAM_HELP", status="REJECTED")
    check("help request closes by itself once its task is completed",
          closed and "automatically" in closed[0]["decision_note"], closed)
    pf = task_by_title(sneha, "Update PFMEA for the new welding cell")
    s, r = call("POST", "/requests", {"type": "TEAM_HELP", "parent_task_id": pf["id"], "title": "Night-shift weld data for the PFMEA",
                                      "needed_by": "2030-01-01", "suggested_assignee_id": manoj_u["id"]}, sneha)
    check("member asks a colleague for help", s == 200, r)
    help_rq = {"id": r.get("id")}
    prop = find(sr["requests"], type="SELF_PROPOSAL", status="PENDING")[0]
    s, r = call("POST", f"/requests/{help_rq['id']}/approve", {"assignee_id": sneha_u["id"]}, rahul)
    check("help cannot be given to the person asking", s == 400, r)
    s, r = call("POST", f"/requests/{help_rq['id']}/approve", {"assignee_id": manoj_u["id"]}, rahul)
    check("manager approves a help request", s == 200, r)
    ht = [t for t in sync(sneha)["tasks"] if t["id"] == r["task_id"]]
    check("help task appears for the requester under her task", ht and ht[0]["parent_id"] == pf["id"], ht)
    s, r = call("POST", f"/requests/{prop['id']}/approve", {}, sneha)
    check("member cannot approve proposals", s == 403, r)
    s, r = call("POST", f"/requests/{prop['id']}/approve", {"note": "Go ahead"}, rahul)
    pt = [t for t in sync(manoj)["tasks"] if t["id"] == r.get("task_id")]
    check("approved proposal becomes a normal task", s == 200 and pt and pt[0]["status"] == "TODO", (s, r, pt))
    s, r = call("POST", "/requests", {"type": "SELF_PROPOSAL", "project_id": weld["project_id"], "title": "Kaizen idea",
                                      "due_date": "2030-01-10"}, manoj)
    rid = r["id"]
    s, r = call("POST", f"/requests/{rid}/withdraw", {}, manoj)
    check("member withdraws own proposal", s == 200, r)
    prdr_id = find(sync(vik)["projects"], code="PRDR")[0]["id"]   # routine work: visible by team, no member list
    s, r = call("POST", "/requests", {"type": "SELF_PROPOSAL", "project_id": prdr_id, "title": "Fixture rack idea",
                                      "due_date": "2030-01-12"}, manoj)
    prid = r["id"]
    rs = sync(rahul)
    ptask = task_by_title(rahul, "Fixture rack idea")
    s, r = call("POST", f"/requests/{prid}/approve", {"assignee_id": pooja_u["id"]}, vik)
    d = sync(rahul, rs["cursor"], rs["scope_ver"])
    check("a proposal given to another team leaves the first team's head", s == 200 and ptask and ptask["id"] in d["revoked"]["tasks"],
          (s, r, d.get("revoked")))
    check("... and reaches the other team's head", task_by_title(suresh, "Fixture rack idea") is not None)

    sq = sync(neha)
    xr = find(sq["requests"], type="CROSS_TEAM", status="PENDING")[0]
    s, r = call("POST", f"/requests/{xr['id']}/approve", {"assignee_id": amit_u["id"]}, lakshmi)
    check("sub-dept head cannot decide requests for the whole department", s == 403, r)
    s, r = call("POST", f"/requests/{xr['id']}/approve", {"assignee_id": priya_u["id"]}, neha)
    check("cross-team work must go to someone in the target team", s in (400, 403), r)
    s, r = call("POST", f"/requests/{xr['id']}/approve", {"assignee_id": amit_u["id"], "due_date": "2030-02-01"}, neha)
    check("target head approves and assigns", s == 200, r)
    xt = r["task_id"]
    check("cross-team task on the requester's dashboard", any(t["id"] == xt for t in sync(priya)["tasks"]))
    check("cross-team task on the doer's dashboard", any(t["id"] == xt for t in sync(amit)["tasks"]))
    s, r = call("POST", "/requests", {"type": "CROSS_TEAM", "target_dept_id": IQC, "project_id": xr["project_id"],
                                      "title": "Check 5 cells", "needed_by": "2030-01-05"}, priya)
    check("request to a sub-department", s == 200, r)
    check("sub-department head notified", [x for x in notifs(lakshmi) if x["type"] == "CROSS_TEAM_REQUEST" and x["request_id"] == r["id"]])
    s, r = call("POST", f"/requests/{r['id']}/approve", {"assignee_id": lakshmi_u["id"]}, neha)
    check("department head may also decide a sub-department request", s == 200, r)
    mq = find(sync(imran)["requests"], type="CROSS_TEAM", status="PENDING", requested_by=anita_u["id"])[0]
    s, r = call("POST", f"/requests/{mq['id']}/approve", {"assignee_id": rahul_u["id"]}, imran)
    check("deputy approves a management request and assigns a sub-dept head", s == 200, r)

    pf = task_by_title(sneha, "Update PFMEA for the new welding cell")
    ext = find(sync(rahul)["extensions"], task_id=pf["id"], status="PENDING")[0]
    s, r = call("POST", f"/extensions/{ext['id']}/approve", {}, sneha)
    check("member cannot approve own extension", s == 403, r)
    s, r = call("POST", f"/extensions/{ext['id']}/approve", {"note": "OK"}, rahul)
    check("manager approves extension -> new due date", s == 200 and task_by_title(sneha, pf["title"])["due_date"] == ext["proposed_due"], r)
    s, r = call("POST", f"/tasks/{pf['id']}/extensions", {"proposed_due": "2000-01-01", "reason": "x"}, sneha)
    check("extension must move the date later", s == 400, r)

    print("== projects")
    s, r = call("POST", "/projects", {"code": "L2KZN", "name": "Line 2 Kaizen", "dept_id": L2}, rahul)
    check("sub-dept head creates a project for his unit", s == 200, r)
    s, r = call("POST", "/projects", {"code": "SNEHA", "name": "x", "dept_id": L2}, sneha)
    check("member cannot create projects", s == 403, r)
    s, r = call("PATCH", f"/projects/{weld['project_id']}", {"status": "CLOSED"}, vik)
    check("project with open tasks cannot be closed", s == 400, r)

    print("== instant updates (long-poll)")
    res = {}

    def waiter(tok, cur, scope, key, wait=20):
        t0 = time.time()
        res[key] = (sync(tok, cur, scope, wait=wait), time.time() - t0)

    st = sync(rahul)
    th = threading.Thread(target=waiter, args=(rahul, st["cursor"], st["scope_ver"], "r"))
    th.start()
    time.sleep(0.5)
    t_send = time.time()
    call("POST", f"/tasks/{weld['id']}/comments", {"body": "Please update the SOP today"}, vik)
    th.join()
    d, dt = res["r"]
    lag = time.time() - t_send
    check("change reaches the other user instantly (< 1 s)", dt < 5 and lag < 1.0 and any(c["body"] == "Please update the SOP today"
                                                                                       for c in d.get("comments", [])), (lag, d.keys()))
    st = sync(rahul)
    th = threading.Thread(target=waiter, args=(rahul, st["cursor"], st["scope_ver"], "r2", 2))
    th.start()
    time.sleep(0.3)
    call("POST", f"/tasks/{l1['id']}/comments", {"body": "Line 1 only"}, suresh)
    th.join()
    d, dt = res["r2"]
    check("unrelated change does not wake other users", d.get("empty") and dt >= 1.8, (dt, d))
    # project members keep seeing a project's work after a task moves on - so losing access is checked on routine work
    s, r = call("POST", "/tasks", {"project_id": prdr_id, "title": "Operator training - shift C", "due_date": "2030-01-20",
                                   "assignee_id": manoj_u["id"], "dept_id": L2}, vik)
    call("POST", f"/tasks/{r['id']}/comments", {"body": "Shift C roster attached"}, vik)
    ot = task_by_title(manoj, "Operator training - shift C")
    st = sync(manoj)
    stp = sync(pooja)
    s, r = call("PATCH", f"/tasks/{ot['id']}", {"assignee_id": pooja_u["id"], "dept_id": L1}, vik)
    check("an edit without row_version is refused", s == 400, r)
    s, r = call("PATCH", f"/tasks/{ot['id']}", {"assignee_id": pooja_u["id"], "dept_id": L1, "row_version": ot["row_version"]}, vik)
    check("department head reassigns across sub-departments", s == 200, r)
    d = sync(manoj, st["cursor"], st["scope_ver"])
    check("old assignee's view drops the task (revoked)", ot["id"] in d["revoked"]["tasks"], d["revoked"])
    d = sync(pooja, stp["cursor"], stp["scope_ver"])
    check("new assignee receives the task with its history", any(t["id"] == ot["id"] for t in d["tasks"]) and
          any(c["task_id"] == ot["id"] for c in d["comments"]), d.keys())
    # lost access is told only to those who had it; comment text only reaches people who can open the task
    call("POST", f"/tasks/{ot['id']}/comments", {"body": "Taking this over from Manoj"}, pooja)
    am = sync(amit)
    pn = sync(pooja)
    s, r = call("PATCH", f"/tasks/{ot['id']}", {"assignee_id": manoj_u["id"], "dept_id": L2,
                                                 "row_version": task_by_title(vik, ot["title"])["row_version"]}, vik)
    d = sync(amit, am["cursor"], am["scope_ver"])
    check("people who never saw a task hear nothing about it", d.get("empty") and not d["revoked"]["tasks"], d.get("revoked"))
    d = sync(pooja, pn["cursor"], pn["scope_ver"])
    check("the previous assignee loses the task again", ot["id"] in d["revoked"]["tasks"], d["revoked"])
    pn = sync(pooja)
    call("POST", f"/tasks/{ot['id']}/comments", {"body": "Internal note after the handover"}, vik)
    check("a former assignee no longer receives the task's comments",
          not [x for x in notifs(pooja, pn["cursor"]) if x["type"] == "COMMENT" and x["task_id"] == ot["id"]])

    print("== independent review findings (v1.0.1)")
    deep = ('{"email":' + "[" * 60000 + "]" * 60000 + ',"password":"x"}').encode()
    s, r = call("POST", "/auth/login", raw=deep)
    check("deeply nested JSON is refused and the server keeps running", s == 400 and call("GET", "/health")[0] == 200, (s, r))
    depts = {x["code"]: x["id"] for x in sync(admin)["departments"]}
    gen3 = find(sync(arjun)["projects"], code="GEN3")[0]["id"]
    s, r = call("POST", "/requests", {"type": "CROSS_TEAM", "target_dept_id": depts["MEC"], "project_id": gen3,
                                      "title": "Bracket redesign", "needed_by": "2030-01-10"}, arjun)
    s, r = call("POST", f"/requests/{r['id']}/approve", {"assignee_id": kavya_u["id"], "due_date": "2030-01-10"}, farhan)
    xt = r.get("task_id")
    nikhil_id = find(sync(admin)["users"], email="nikhil@demo.local")[0]["id"]
    s, r = call("POST", "/tasks", {"parent_id": xt, "title": "Sneak sub-task", "due_date": "2030-01-05", "assignee_id": nikhil_id}, arjun)
    check("a requesting head cannot add sub-tasks to the other team's task", s == 403, r)
    s, r = call("POST", "/tasks", {"project_id": ot["project_id"], "title": "Label printer check", "due_date": "2030-02-01",
                                   "assignee_id": sneha_u["id"]}, rahul)
    tid = r["id"]
    call("POST", f"/tasks/{tid}/extensions", {"proposed_due": "2030-03-01", "reason": "Printer arrives late"}, sneha)
    call("POST", f"/tasks/{tid}/submit", {"note": "Checked"}, sneha)
    call("POST", f"/tasks/{tid}/approve", {}, rahul)
    ex = [e for e in sync(rahul)["extensions"] if e["task_id"] == tid]
    check("finishing a task closes its open extension request", ex and ex[0]["status"] == "REJECTED", ex)
    prdr = find(sync(vik)["projects"], code="PRDR")[0]["id"]
    s, r = call("PATCH", f"/tasks/{weld['id']}", {"project_id": prdr, "row_version": task_by_title(vik, weld["title"])["row_version"]}, vik)
    subs = [t for t in sync(vik)["tasks"] if t.get("parent_id") == weld["id"]]
    check("moving a main task moves its sub-tasks with it", s == 200 and subs and all(t["project_id"] == prdr for t in subs), r)
    s, r = call("POST", "/tasks", {"project_id": task_by_title(anita, "Line balancing simulation")["project_id"],
                                   "title": "Investor update", "due_date": "2030-02-01"}, anita)
    s, r = call("PATCH", f"/tasks/{r['id']}", {"due_date": "2030-02-15",
                                               "row_version": task_by_title(anita, "Investor update")["row_version"]}, anita)
    check("the head of Management re-plans her own task", s == 200, r)
    st = sync(lakshmi)
    out = {}

    def w(k):
        t0 = time.time()
        out[k] = (sync(lakshmi, st["cursor"], st["scope_ver"], wait=3), time.time() - t0)
    ths = [threading.Thread(target=w, args=(i,)) for i in range(3)]
    for t in ths:
        t.start()
        time.sleep(0.2)
    for t in ths:
        t.join()
    quick = [k for k, (d, dt) in out.items() if dt < 1.0 and d.get("retry_after")]
    check("one session can hold at most two waiting requests", len(quick) == 1,
          {k: (round(dt, 2), d.get("retry_after")) for k, (d, dt) in out.items()})
    # ... and one person at most four over all their PCs
    toks = [lakshmi, login("lakshmi@demo.local")[0], login("lakshmi@demo.local")[0]]
    st = sync(lakshmi)
    out = {}

    def w2(k, tok):
        t0 = time.time()
        out[k] = (sync(tok, st["cursor"], st["scope_ver"], wait=3), time.time() - t0)
    ths = [threading.Thread(target=w2, args=(i, toks[i // 2])) for i in range(5)]
    for t in ths:
        t.start()
        time.sleep(0.2)
    for t in ths:
        t.join()
    quick = [k for k, (d, dt) in out.items() if dt < 1.0 and d.get("retry_after")]
    check("one person can hold at most four waiting requests over several PCs", quick == [4],
          {k: (round(dt, 2), d.get("retry_after")) for k, (d, dt) in out.items()})
    # completing the last sub-task: the main task's owner gets one message, not two
    s, r = call("POST", "/tasks", {"project_id": prdr, "title": "Fixture audit", "due_date": "2030-03-01",
                                   "assignee_id": sneha_u["id"]}, rahul)
    fa = r["id"]
    s, r = call("POST", "/tasks", {"parent_id": fa, "title": "Count fixtures", "due_date": "2030-02-01",
                                   "assignee_id": manoj_u["id"]}, rahul)
    fsub = r["id"]
    ns = sync(sneha)
    call("POST", f"/tasks/{fsub}/submit", {"note": "Counted 42"}, manoj)
    s, r = call("POST", f"/tasks/{fsub}/approve", {}, rahul)
    got = [x["type"] for x in notifs(sneha, ns["cursor"]) if x["task_id"] in (fa, fsub)]
    check("the main-task owner gets one message when the last sub-task is done",
          s == 200 and got.count("SUBTASKS_DONE") == 1 and "TASK_COMPLETED" not in got, (s, r, got))
    # old work: a full download keeps the breakdown of open tasks; closed / old work comes on demand
    import sqlite3
    weld_subs = [t["id"] for t in sync(vik)["tasks"] if t.get("parent_id") == weld["id"]]
    calib = task_by_title(vik, "Calibrate torque tools")
    db = sqlite3.connect(os.path.join(WORK, "taskflow.db"), timeout=10)
    db.execute(f"UPDATE tasks SET updated_at='2020-01-01T00:00:00Z' WHERE status='DONE' AND "
               f"(parent_id={weld['id']} OR id={calib['id']})")
    db.commit()
    db.close()
    full_v = {t["id"] for t in sync(vik)["tasks"]}
    check("a full download keeps every sub-task of an open main task, however old", set(weld_subs) <= full_v,
          set(weld_subs) - full_v)
    check("a full download leaves out old finished routine work", calib["id"] not in full_v)
    s, r = call("GET", f"/projects/{calib['project_id']}/tasks", token=vik)
    check("old work of a project is loaded on demand", s == 200 and calib["id"] in {t["id"] for t in r["tasks"]}, s)
    s, r = call("GET", f"/projects/{calib['project_id']}/tasks", token=sneha)
    check("... with the same visibility rules", s == 200 and calib["id"] not in {t["id"] for t in r["tasks"]}, s)
    s, r = call("PATCH", f"/admin/departments/{PRD}", {"active": 0}, admin)
    check("a department with active sub-departments cannot be deactivated", s == 400 and "sub-department" in r.get("error", ""), r)
    call("POST", "/projects", {"code": "TESTR", "name": "Code clash", "dept_id": depts["IT"]}, admin)
    s, r = call("POST", "/admin/departments", {"code": "TEST", "name": "Test team"}, admin)
    check("a department code that clashes with a project code is refused cleanly", s == 409, r)

    print("== v1.1: project members, helpers, dependencies, milestones, repeats, mentions, help in the department")
    amit_id = amit_u["id"]
    s, r = call("POST", "/projects", {"code": "PACK", "name": "Pack line upgrade", "dept_id": PRD,
                                      "members": [sneha_u["id"], pooja_u["id"]]}, vik)
    check("a project is created with members", s == 200, r)
    pack = r["id"]
    d = sync(sneha)
    pm = {x["user_id"] for x in d["project_members"] if x["project_id"] == pack and x["active"]}
    check("members (and the owner) are listed for project members", pm == {sneha_u["id"], pooja_u["id"], vik_u["id"]}, pm)
    check("a member sees the new project", any(p["id"] == pack for p in d["projects"]))
    s, r = call("POST", "/tasks", {"project_id": pack, "title": "Rework conveyor guards", "due_date": "2030-04-30",
                                   "assignee_id": manoj_u["id"], "dept_id": L2, "is_milestone": 1}, vik)
    guards = r["id"]
    check("whoever is given a task joins the project", any(x["project_id"] == pack and x["user_id"] == manoj_u["id"]
                                                         for x in sync(vik)["project_members"]))
    check("a milestone is stored", task_by_title(vik, "Rework conveyor guards")["is_milestone"] == 1)
    check("members see a colleague's task in their project", task_by_title(sneha, "Rework conveyor guards") is not None)
    check("... from another team too", task_by_title(pooja, "Rework conveyor guards") is not None)
    s1 = call("POST", f"/tasks/{guards}/start", {}, sneha)[0]
    s2 = call("POST", f"/tasks/{guards}/cancel", {"comment": "x"}, sneha)[0]
    s3 = call("PATCH", f"/tasks/{guards}", {"title": "x", "row_version": task_by_title(vik, "Rework conveyor guards")["row_version"]}, sneha)[0]
    check("members cannot act on others' tasks (start, cancel, edit)", (s1, s2, s3) == (403, 403, 403), (s1, s2, s3))
    check("members may comment on the project's tasks", call("POST", f"/tasks/{guards}/comments", {"body": "Guards drawing is on the share"}, sneha)[0] == 200)
    check("people outside the project do not see its work", task_by_title(amit, "Rework conveyor guards") is None and
          call("GET", f"/tasks/{guards}", token=amit)[0] == 404)
    check("only the owner or department managers add members",
          call("POST", f"/projects/{pack}/members", {"user_ids": [amit_id]}, sneha)[0] == 403)
    check("routine work has no member list",
          call("POST", f"/projects/{prdr_id}/members", {"user_ids": [amit_id]}, vik)[0] == 400)
    check("the owner cannot be removed", call("DELETE", f"/projects/{pack}/members/{vik_u['id']}", None, vik)[0] == 400)
    pst = sync(pooja)
    s, r = call("DELETE", f"/projects/{pack}/members/{pooja_u['id']}", None, vik)
    d = sync(pooja, pst["cursor"], pst["scope_ver"])
    check("a removed member's app downloads everything again, without the project's work",
          s == 200 and d["full"] and not any(t["id"] == guards for t in d["tasks"]), (s, d.get("full")))

    # people who also work on a task (the assignee stays responsible)
    ns = sync(sneha)
    s, r = call("PATCH", f"/tasks/{guards}", {"helpers": [sneha_u["id"]], "row_version": task_by_title(vik, "Rework conveyor guards")["row_version"]}, vik)
    d = sync(sneha, ns["cursor"], ns["scope_ver"])
    check("a manager adds a colleague who also works on the task", s == 200 and
          any(x["task_id"] == guards and x["user_id"] == sneha_u["id"] and x["active"] for x in d["task_members"]), (s, r))
    check("... who is told", [x for x in d.get("notifications", []) if x["type"] == "HELPER_ADDED" and x["task_id"] == guards])
    check("a helper cannot mark the task complete", call("POST", f"/tasks/{guards}/submit", {"note": "done"}, sneha)[0] == 403)
    ns = sync(sneha)
    call("POST", f"/tasks/{guards}/comments", {"body": "Started on the left side"}, manoj)
    check("helpers hear about comments", [x for x in notifs(sneha, ns["cursor"]) if x["type"] == "COMMENT" and x["task_id"] == guards])
    s, r = call("PATCH", f"/tasks/{guards}", {"helpers": [sneha_u["id"], pooja_u["id"]],
                                              "row_version": task_by_title(rahul, "Rework conveyor guards")["row_version"]}, rahul)
    check("a sub-department head cannot add someone from another team", s == 403, r)
    s, r = call("POST", "/tasks", {"project_id": prdr_id, "title": "Kitting rack 5S", "due_date": "2030-02-20",
                                   "assignee_id": manoj_u["id"], "dept_id": L2, "helpers": [sneha_u["id"]]}, rahul)
    kit = r["id"]
    check("on routine work a helper sees the task she helps on", s == 200 and task_by_title(sneha, "Kitting rack 5S") is not None, r)
    ns = sync(sneha)
    call("PATCH", f"/tasks/{kit}", {"helpers": [], "row_version": task_by_title(rahul, "Kitting rack 5S")["row_version"]}, rahul)
    d = sync(sneha, ns["cursor"], ns["scope_ver"])
    check("removing a helper takes the routine task away from her", d["full"] and not any(t["id"] == kit for t in d["tasks"]), d.get("full"))

    # dependencies (finish-to-start)
    s, r = call("POST", "/tasks", {"project_id": pack, "title": "Order guard panels", "due_date": "2030-03-15",
                                   "assignee_id": manoj_u["id"], "dept_id": L2}, vik)
    order = r["id"]
    s, r = call("POST", "/tasks", {"project_id": pack, "title": "Fit guard panels", "due_date": "2030-04-15",
                                   "assignee_id": sneha_u["id"], "dept_id": L2, "depends_on": [order]}, vik)
    fit = r["id"]
    deps = [x for x in sync(sneha)["task_deps"] if x["task_id"] == fit and x["active"]]
    check("a task can wait for another", s == 200 and deps and deps[0]["depends_on"] == order, (s, r, deps))
    s, r = call("PATCH", f"/tasks/{order}", {"depends_on": [fit], "row_version": task_by_title(vik, "Order guard panels")["row_version"]}, vik)
    check("waiting in a circle is refused", s == 400 and "circle" in r.get("error", ""), r)
    s, r = call("PATCH", f"/tasks/{fit}", {"depends_on": [kit], "row_version": task_by_title(vik, "Fit guard panels")["row_version"]}, vik)
    check("waiting for a task of another project is refused", s == 400, r)
    ns = sync(sneha)
    call("POST", f"/tasks/{order}/submit", {"note": "Panels ordered, PO 4471"}, manoj)
    s, r = call("POST", f"/tasks/{order}/approve", {}, rahul)
    check("when it is done, the waiting task's owner hears she can start",
          s == 200 and [x for x in notifs(sneha, ns["cursor"]) if x["type"] == "CAN_START" and x["task_id"] == fit], (s, r))

    # repeating tasks
    s, r = call("POST", "/tasks", {"project_id": prdr_id, "title": "Weekly torque check", "due_date": "2030-03-04",
                                   "start_date": "2030-03-03", "assignee_id": manoj_u["id"], "dept_id": L2, "recur": "WEEKLY"}, rahul)
    wk = r["id"]
    call("POST", f"/tasks/{wk}/submit", {"note": "All tools within 2 %"}, manoj)
    call("POST", f"/tasks/{wk}/approve", {}, rahul)
    series = [t for t in sync(rahul)["tasks"] if t["title"] == "Weekly torque check"]
    nxt = [t for t in series if t["id"] != wk]
    check("a weekly task creates the next one when it is done",
          len(nxt) == 1 and nxt[0]["due_date"] == "2030-03-11" and nxt[0]["start_date"] == "2030-03-10" and nxt[0]["status"] == "TODO"
          and nxt[0]["recur"] == "WEEKLY", [(t["due_date"], t["status"], t["recur"]) for t in series])
    s, r = call("POST", "/tasks", {"project_id": prdr_id, "title": "Monthly gauge audit", "due_date": "2030-01-31",
                                   "assignee_id": manoj_u["id"], "dept_id": L2, "recur": "MONTHLY"}, rahul)
    call("POST", f"/tasks/{r['id']}/submit", {"note": "ok"}, manoj)
    call("POST", f"/tasks/{r['id']}/approve", {}, rahul)
    feb = [t["due_date"] for t in sync(rahul)["tasks"] if t["title"] == "Monthly gauge audit" and t["status"] == "TODO"]
    check("a monthly task due on the 31st comes back on the last day of the next month", feb == ["2030-02-28"], feb)
    for _ in range(2):  # Feb 28 -> Mar 31 -> Apr 30: the series keeps to month ends, it does not drift to the 28th
        cur = [t for t in sync(rahul)["tasks"] if t["title"] == "Monthly gauge audit" and t["status"] == "TODO"][0]
        call("POST", f"/tasks/{cur['id']}/submit", {"note": "ok"}, manoj)
        call("POST", f"/tasks/{cur['id']}/approve", {}, rahul)
    dues = sorted(t["due_date"] for t in sync(rahul)["tasks"] if t["title"] == "Monthly gauge audit")
    check("a monthly series due on the 31st stays on month ends", dues == ["2030-01-31", "2030-02-28", "2030-03-31", "2030-04-30"], dues)
    s, r = call("POST", "/tasks", {"project_id": prdr_id, "title": "Main with subs", "due_date": "2030-05-31",
                                   "assignee_id": rahul_u["id"], "dept_id": L2}, rahul)
    main_r = r["id"]
    s, r = call("POST", "/tasks", {"parent_id": main_r, "title": "Weekly sub", "due_date": "2030-05-10", "assignee_id": manoj_u["id"],
                                   "dept_id": L2, "recur": "WEEKLY"}, rahul)
    check("a sub-task cannot repeat (it would keep its main task open)", s == 400 and "main tasks" in r.get("error", ""), (s, r))
    s, r = call("POST", "/tasks", {"parent_id": main_r, "title": "Plain sub", "due_date": "2030-05-10", "assignee_id": manoj_u["id"],
                                   "dept_id": L2}, rahul)
    sub_r = r["id"]
    tv = call("GET", f"/tasks/{sub_r}", None, rahul)[1]["task"]
    s, r = call("PATCH", f"/tasks/{sub_r}", {"recur": "WEEKLY", "row_version": tv["row_version"]}, rahul)
    check("a sub-task cannot be set to repeat later either", s == 400, (s, r))
    s, r = call("POST", "/tasks", {"project_id": prdr_id, "title": "Daily line walk", "due_date": "2020-01-01",
                                   "assignee_id": manoj_u["id"], "dept_id": L2, "recur": "DAILY"}, rahul)
    call("POST", f"/tasks/{r['id']}/submit", {"note": "ok"}, manoj)
    call("POST", f"/tasks/{r['id']}/approve", {}, rahul)
    today = sync(rahul)["today"]
    dl = [t["due_date"] for t in sync(rahul)["tasks"] if t["title"] == "Daily line walk" and t["status"] == "TODO"]
    check("a late repeating task comes back from today, not in the past", dl == [today], (dl, today))
    check("repeat must be daily, weekly or monthly",
          call("POST", "/tasks", {"project_id": prdr_id, "title": "x", "due_date": "2030-01-01", "assignee_id": manoj_u["id"],
                                  "dept_id": L2, "recur": "HOURLY"}, rahul)[0] == 400)

    # @mentions
    ms, am = sync(manoj), sync(amit)
    call("POST", f"/tasks/{fit}/comments", {"body": "@Manoj Patil can you share the panel drawing? cc @Amit Kumar"}, sneha)
    mn = [x["type"] for x in notifs(manoj, ms["cursor"]) if x["task_id"] == fit]
    check("a mentioned colleague is notified once, as a mention", mn.count("MENTION") == 1 and "COMMENT" not in mn, mn)
    check("someone who cannot open the task is not notified", not [x for x in notifs(amit, am["cursor"]) if x["task_id"] == fit])

    # help from another team of the same department
    s, r = call("POST", "/tasks", {"project_id": pack, "title": "Validate guard interlocks", "due_date": "2030-05-10",
                                   "assignee_id": sneha_u["id"], "dept_id": L2}, vik)
    gi = r["id"]
    vs, rs = sync(vik), sync(rahul)
    s, r = call("POST", "/requests", {"type": "TEAM_HELP", "parent_task_id": gi, "title": "Interlock test rig from Line 1",
                                      "needed_by": "2030-05-01", "suggested_assignee_id": pooja_u["id"]}, sneha)
    hq = r.get("id")
    check("a member asks a colleague of another team in her department", s == 200, r)
    check("... the department head is asked to decide", [x for x in notifs(vik, vs["cursor"]) if x["type"] == "HELP_REQUEST" and x["request_id"] == hq])
    check("... the asking team's head is kept informed and sees the request",
          [x for x in notifs(rahul, rs["cursor"]) if x["type"] == "HELP_ASKED" and x["task_id"] == gi]
          and any(q["id"] == hq for q in sync(rahul)["requests"]))
    rs = sync(rahul)
    check("... her own team's head cannot assign someone of another team",
          call("POST", f"/requests/{hq}/approve", {"assignee_id": pooja_u["id"]}, rahul)[0] == 403)
    ss = sync(suresh)
    s, r = call("POST", f"/requests/{hq}/approve", {"assignee_id": pooja_u["id"]}, vik)
    ht = task_by_title(vik, "Interlock test rig from Line 1")
    check("the department head assigns the colleague", s == 200 and ht and ht["dept_id"] == L1 and ht["parent_id"] == gi, (s, r))
    check("... her head is told where she helps", [x for x in notifs(suresh, ss["cursor"]) if x["type"] == "HELP_GIVEN"])
    check("... and the asking team's head sees the help", task_by_title(rahul, "Interlock test rig from Line 1") is not None)
    check("... and is told who will help", [x for x in notifs(rahul, rs["cursor"]) if x["type"] == "HELP_GIVEN"])
    s, r = call("POST", "/requests", {"type": "TEAM_HELP", "parent_task_id": gi, "title": "x", "suggested_assignee_id": amit_id}, sneha)
    check("help from another department goes through 'Request work from another team'", s == 400 and "another team" in r.get("error", ""), r)

    # ---- who may use a project; membership that sticks or ends; helpers; dependencies on hidden work
    print("== v1.1: project access and membership rules")
    meera, meera_u = login("meera@demo.local")
    allp = sync(admin)["projects"]
    line2_id, gen3_id, cellq_id = (find(allp, code=c)[0]["id"] for c in ("LINE2", "GEN3", "CELLQ"))
    s, r = call("POST", "/tasks", {"project_id": line2_id, "title": "x", "due_date": "2030-01-10", "assignee_id": neha_u["id"],
                                   "dept_id": QUA}, neha)
    check("a head cannot put work into another department's project (and so join it)", s == 403 and "not part of" in r.get("error", ""), r)
    s, r = call("POST", "/requests", {"type": "SELF_PROPOSAL", "project_id": gen3_id, "title": "x", "due_date": "2030-02-01"}, amit)
    check("nobody proposes work in a project they are not part of", s == 403 and "not part of" in r.get("error", ""), r)
    s, r = call("POST", "/requests", {"type": "CROSS_TEAM", "target_dept_id": MEC, "project_id": cellq_id, "title": "Bracket",
                                      "needed_by": "2030-03-01"}, meera)
    check("a request to another team cannot name a project the requester is not part of", s == 403 and "not part of" in r.get("error", ""), r)
    s, r = call("POST", "/tasks", {"project_id": find(allp, code="QUAR")[0]["id"] if find(allp, code="QUAR") else prdr_id,
                                   "title": "IQC plan for moving", "due_date": "2030-02-10", "assignee_id": lakshmi_u["id"]}, neha)
    lt = task_by_title(neha, "IQC plan for moving")
    s, r = call("PATCH", f"/tasks/{lt['id']}", {"project_id": gen3_id, "row_version": lt["row_version"]}, neha)
    check("work cannot be moved into a project one is not part of", s == 403 and "not part of" in r.get("error", ""), r)

    s, r = call("POST", "/projects", {"code": "SECR", "name": "Confidential layout", "dept_id": PRD}, vik)
    secr = r["id"]
    call("POST", "/tasks", {"project_id": secr, "title": "Layout options", "due_date": "2030-06-30", "assignee_id": manoj_u["id"],
                            "dept_id": L2}, vik)
    s, r = call("POST", "/tasks", {"project_id": secr, "title": "Floor measurements", "due_date": "2030-06-30",
                                   "assignee_id": pooja_u["id"], "dept_id": L1}, vik)
    fm = r["id"]
    check("given a task by the owner, a person sees the project's other work", task_by_title(pooja, "Layout options") is not None)
    call("DELETE", f"/projects/{secr}/members/{pooja_u['id']}", None, vik)
    s, r = call("POST", "/tasks", {"parent_id": fm, "title": "Measure bay 3", "due_date": "2030-06-01", "assignee_id": pooja_u["id"]}, pooja)
    check("a removed member does not come back by adding a sub-task to her own task",
          s == 200 and task_by_title(pooja, "Layout options") is None, (s, r))
    check("... she keeps her own task", task_by_title(pooja, "Floor measurements") is not None)
    call("POST", f"/projects/{secr}/members", {"user_ids": [pooja_u["id"]]}, vik)
    check("added again by hand, she sees the project again", task_by_title(pooja, "Layout options") is not None)
    s, r = call("POST", "/tasks", {"project_id": secr, "title": "Cable route check", "due_date": "2030-06-20",
                                   "assignee_id": sneha_u["id"], "dept_id": L2}, vik)
    crc = r["id"]
    check("(she sees the project while she has work in it)", task_by_title(sneha, "Layout options") is not None)
    t = task_by_title(vik, "Cable route check")
    call("PATCH", f"/tasks/{crc}", {"assignee_id": manoj_u["id"], "dept_id": L2, "row_version": t["row_version"]}, vik)
    check("when her only task there goes to someone else, the project's work leaves her app",
          task_by_title(sneha, "Layout options") is None and task_by_title(sneha, "Cable route check") is None)
    s, r = call("POST", "/tasks", {"project_id": secr, "title": "Guard drawings", "due_date": "2030-06-25",
                                   "assignee_id": manoj_u["id"], "dept_id": L2, "helpers": [sneha_u["id"]]}, vik)
    check("a helper sees the task she helps on, not the rest of the project",
          s == 200 and task_by_title(sneha, "Guard drawings") is not None and task_by_title(sneha, "Layout options") is None, (s, r))
    s, r = call("POST", "/tasks", {"project_id": secr, "title": "Short job", "due_date": "2030-06-21", "assignee_id": suresh_u["id"], "dept_id": L1}, vik)
    sj = r["id"]
    seen = task_by_title(suresh, "Layout options") is not None
    call("POST", f"/tasks/{sj}/cancel", {"comment": "Not needed"}, vik)
    check("when a person's only task there is cancelled, the project leaves their app",
          seen and task_by_title(suresh, "Layout options") is None, seen)
    check("people outside a project do not receive its member list",
          not [x for x in sync(suresh)["project_members"] if x["project_id"] == line2_id])
    s, r = call("PATCH", f"/projects/{secr}", {"owner_id": rahul_u["id"]}, vik)
    d = sync(rahul)
    check("a project can be handed to a new owner, who then sees all of it",
          s == 200 and find(d["projects"], id=secr)[0]["owner_id"] == rahul_u["id"] and task_by_title(rahul, "Floor measurements") is not None,
          (s, r))

    print("== v1.2: status changes")
    s, r = call("POST", "/tasks", {"project_id": prdr_id, "title": "Waiting for parts", "due_date": "2030-07-10",
                                   "assignee_id": manoj_u["id"], "dept_id": L2}, rahul)
    wp = r["id"]
    rs = sync(rahul)
    s, r = call("POST", f"/tasks/{wp}/hold", {"comment": "Fixture parts not delivered yet"}, manoj)
    t = task_by_title(rahul, "Waiting for parts")
    check("a task that has not started can be put on hold, with a reason", s == 200 and t["status"] == "ON_HOLD", (s, r))
    check("... and the manager is told", [x for x in notifs(rahul, rs["cursor"]) if x["type"] == "STATUS_CHANGED" and x["task_id"] == wp])
    check("a hold needs a reason", call("POST", f"/tasks/{wp}/resume", {}, manoj)[0] == 200 and
          call("POST", f"/tasks/{wp}/hold", {}, manoj)[0] == 400)
    check("a colleague cannot change someone else's status", call("POST", f"/tasks/{wp}/hold", {"comment": "x"}, sneha)[0] in (403, 404))

    print("== v1.1: dependencies and helpers - edge cases")
    s, r = call("POST", "/tasks", {"project_id": prdr_id, "title": "RV main", "due_date": "2030-07-31", "assignee_id": manoj_u["id"],
                                   "dept_id": L2}, rahul)
    rvm = r["id"]
    rv1 = call("POST", "/tasks", {"parent_id": rvm, "title": "RV sub 1", "due_date": "2030-07-01", "assignee_id": manoj_u["id"],
                                  "dept_id": L2}, rahul)[1]["id"]
    call("POST", "/tasks", {"parent_id": rvm, "title": "RV sub 2", "due_date": "2030-07-01", "assignee_id": manoj_u["id"], "dept_id": L2}, rahul)
    before = task_by_title(rahul, "RV sub 2")["row_version"]
    s, r = call("PATCH", f"/tasks/{rv1}", {"helpers": [sneha_u["id"]], "row_version": task_by_title(rahul, "RV sub 1")["row_version"]}, rahul)
    check("adding a helper does not make an edit of a sibling task fail", s == 200 and task_by_title(rahul, "RV sub 2")["row_version"] == before,
          (s, r))
    s, r = call("POST", "/tasks", {"project_id": prdr_id, "title": "L1 fixture check", "due_date": "2030-03-01",
                                   "assignee_id": pooja_u["id"], "dept_id": L1}, vik)
    l1fx = r["id"]
    s, r = call("POST", "/tasks", {"project_id": prdr_id, "title": "L2 fixture swap", "due_date": "2030-03-10",
                                   "assignee_id": manoj_u["id"], "dept_id": L2, "depends_on": [l1fx]}, vik)
    l2fx = r["id"]
    rows = [x for x in sync(rahul)["task_deps"] if x["task_id"] == l2fx and x["active"]]
    check("a dependency on a task one cannot open arrives without its id",
          len(rows) == 1 and rows[0]["depends_on"] == 0 and rows[0].get("hidden") == 1 and rows[0].get("done") == 0, rows)
    det = call("GET", f"/tasks/{l2fx}", token=manoj)[1]["task_deps"]
    check("... also in the task's details", det and all(x["depends_on"] == 0 for x in det), det)
    t = task_by_title(rahul, "L2 fixture swap")
    s, r = call("PATCH", f"/tasks/{l2fx}", {"title": "L2 fixture swap (both shifts)", "depends_on": [], "helpers": [],
                                            "row_version": t["row_version"]}, rahul)
    still = [x for x in sync(vik)["task_deps"] if x["task_id"] == l2fx and x["active"]]
    check("the task's manager can edit it; the dependency he cannot see stays",
          s == 200 and len(still) == 1 and still[0]["depends_on"] == l1fx, (s, r, still))
    ms, rs = sync(manoj), sync(rahul)
    call("POST", f"/tasks/{l1fx}/cancel", {"comment": "Fixture replaced"}, vik)
    check("when the awaited task is cancelled, the waiting task's owner hears he can start",
          [x for x in notifs(manoj, ms["cursor"]) if x["type"] == "CAN_START" and x["task_id"] == l2fx])
    rows = [x for x in sync(rahul, rs["cursor"], rs["scope_ver"]).get("task_deps", []) if x["task_id"] == l2fx and x["active"]]
    check("... and the hidden dependency shows as finished", rows and rows[0]["done"] == 1, rows)
    s, r = call("POST", "/requests", {"type": "SELF_PROPOSAL", "project_id": pack, "title": "Poka-yoke idea", "due_date": "2030-05-01"}, sneha)
    s2, r2 = call("POST", "/tasks", {"project_id": pack, "title": "Waits for an idea", "due_date": "2030-05-20", "assignee_id": manoj_u["id"],
                                     "dept_id": L2, "depends_on": [r.get("task_id", 0) or 1]}, vik)
    check("a task cannot wait for a mere proposal", s == 200 and s2 == 400, (s, r, s2, r2))
    s, r = call("POST", "/tasks", {"project_id": pack, "title": "Cycle main", "due_date": "2030-08-31", "assignee_id": manoj_u["id"],
                                   "dept_id": L2}, vik)
    cm = r["id"]
    cs = call("POST", "/tasks", {"parent_id": cm, "title": "Cycle sub", "due_date": "2030-08-01", "assignee_id": manoj_u["id"],
                                 "dept_id": L2}, vik)[1]["id"]
    call("POST", "/tasks", {"project_id": pack, "title": "Cycle X", "due_date": "2030-08-15", "assignee_id": manoj_u["id"], "dept_id": L2,
                            "depends_on": [cm]}, vik)
    cx = task_by_title(vik, "Cycle X")["id"]
    s, r = call("PATCH", f"/tasks/{cs}", {"depends_on": [cx], "row_version": task_by_title(vik, "Cycle sub")["row_version"]}, vik)
    check("a circle through the main task is refused", s == 400 and "circle" in r.get("error", ""), r)

    test_tok = tmp_tok
    st = sync(test_tok)
    s, ms = call("GET", "/admin/users", token=admin)
    memb = find(sync(admin)["memberships"], user_id=test_id, dept_id=L2)[0]
    s, r = call("DELETE", f"/admin/memberships/{memb['id']}", None, admin)
    check("admin removes a membership", s == 200, r)
    d = sync(test_tok, st["cursor"], st["scope_ver"])
    check("membership change forces a full refresh for that user", d["full"] is True, d.get("full"))
    d = sync(manoj, 10**12, sync(manoj)["scope_ver"])
    check("app ahead of the server (restored backup) gets a full refresh", d["full"] is True, d.get("full"))

    print("== administration")
    s, r = call("POST", "/admin/departments", {"code": "PRDL3", "name": "Line 3 Assembly", "parent_id": PRD}, admin)
    check("admin creates a sub-department", s == 200, r)
    s, r = call("POST", "/admin/departments", {"code": "PRDL3X", "name": "x", "parent_id": r["id"]}, admin)
    check("sub-sub-departments refused", s == 400, r)
    s, r = call("POST", "/admin/departments", {"code": "PRDL3", "name": "dup"}, admin)
    check("duplicate code refused", s == 409, r)
    s, r = call("PATCH", f"/admin/departments/{L2}", {"active": 0}, admin)
    check("department with open tasks cannot be deactivated", s == 400, r)
    admin_id = find(sync(admin)["users"], email="admin@taskflow.local")[0]["id"]
    s, r = call("POST", f"/admin/users/{admin_id}/disable", {}, admin)
    check("admin cannot disable himself", s == 400, r)
    s, r = call("GET", "/admin/audit?limit=50", token=admin)
    check("audit log records actions", s == 200 and len(r["audit"]) == 50, len(r.get("audit", [])))
    s, r = call("POST", "/admin/backup", {}, admin)
    check("on-demand backup written", s == 200 and os.path.exists(os.path.join(WORK, r["path"].replace("\\", "/"))), r)
    s, r = call("GET", "/admin/system", token=admin)
    check("system info", s == 200 and r["users"] > 20, r)

    print("== concurrency")
    t = task_by_title(rahul, "Cycle-time study for module stacking")
    s, r = call("PATCH", f"/tasks/{t['id']}", {"title": "Cycle-time study (both shifts)", "row_version": t["row_version"]}, rahul)
    check("edit with current version accepted", s == 200, r)
    s, r = call("PATCH", f"/tasks/{t['id']}", {"title": "stale edit", "row_version": t["row_version"]}, rahul)
    check("stale edit refused (409)", s == 409, r)
    s, r = call("PATCH", f"/tasks/{t['id']}", {"progress": 80}, rahul)
    check("progress cannot be set by hand", task_by_title(rahul, "Cycle-time study (both shifts)")["progress"] == 0, r)

    print("== notifications")
    n = notifs(sneha)
    unread = [x["id"] for x in n if not x["is_read"]]
    s, r = call("POST", "/notifications/read", {"ids": unread[:2]}, sneha)
    n2 = {x["id"]: x["is_read"] for x in notifs(sneha)}
    check("mark notifications read", all(n2[i] for i in unread[:2]), n2)
    call("POST", "/notifications/read", {"all": 1}, sneha)
    check("mark all read", all(x["is_read"] for x in notifs(sneha)))

    print("== abuse protection")
    for i in range(20):
        call("POST", "/auth/login", {"email": "nobody@demo.local", "password": "Wrong12345"})
    s, r = call("POST", "/auth/login", {"email": "vikram@demo.local", "password": "Demo@123"})
    check("sign-in blocked from a computer after many failures", s == 429, r)
    codes = []
    def flood():
        for _ in range(400):
            c = call("GET", "/health")[0]
            codes.append(c)
            if c == 429:
                return
    ts = [threading.Thread(target=flood) for _ in range(8)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    check("request flood is throttled (429)", 429 in codes, codes[-5:])
    time.sleep(2.5)
    check("service recovers after the flood", call("GET", "/health")[0] == 200)

    print("== shutdown")
    # a stop request (systemctl stop / service stop) must not wait for held instant-update requests
    time.sleep(1.5)
    tok_s = manoj
    d0 = sync(tok_s)
    def hold():
        try:
            call("GET", f"/sync?cursor={d0['cursor']}&scope={d0['scope_ver']}&wait=25", token=tok_s)
        except Exception:
            pass  # the server went away - expected here
    held = threading.Thread(target=hold)
    held.daemon = True
    held.start()
    time.sleep(1.0)
    t0 = time.time()
    proc.terminate()
    try:
        proc.wait(timeout=15)
    except Exception:
        pass
    check("server stops within a few seconds while instant-update requests are held",
          proc.poll() is not None and time.time() - t0 < 8, f"{time.time() - t0:.1f} s")

    # ---- morning digest (command line; the running server sends it daily at digest_time)
    print("\n== morning digest")
    ini = os.path.join(WORK, "server.ini")
    r = subprocess.run(CMD + ["--config", ini, "--digest-now"], cwd=WORK, capture_output=True, text=True, timeout=120)
    check("digest refuses while e-mail is switched off", r.returncode == 1 and "switched off" in (r.stdout + r.stderr),
          (r.stdout + r.stderr)[-200:])
    with open(ini, "a") as f:
        f.write("email_enabled = 1\nsmtp_host = 127.0.0.1\nsmtp_port = 9\nsmtp_security = none\nsmtp_from = t@x.in\n")
    r = subprocess.run(CMD + ["--config", ini, "--digest-now"], cwd=WORK, capture_output=True, text=True, timeout=120)
    import sqlite3
    dbc = sqlite3.connect(os.path.join(WORK, "taskflow.db"))
    mails = dbc.execute("SELECT to_email, subject, body FROM email_outbox WHERE subject LIKE '[TaskFlow] Your day:%'").fetchall()
    check("digest queues e-mails", r.returncode == 0 and len(mails) > 0, (r.stdout + r.stderr)[-200:])
    tomorrow = time.strftime("%Y-%m-%d", time.localtime(time.time() + 86400))
    def has_reason(email):
        uid, st = dbc.execute("SELECT id, status FROM users WHERE email=?", (email,)).fetchone()
        if st != "ACTIVE":
            return False
        own = dbc.execute("SELECT COUNT(*) FROM tasks WHERE assignee_id=? AND status IN ('TODO','IN_PROGRESS','ON_HOLD') "
                          "AND due_date<>'' AND due_date<=?", (uid, tomorrow)).fetchone()[0]
        lead = dbc.execute("SELECT COUNT(*) FROM memberships WHERE user_id=? AND active=1 AND role IN ('HEAD','DEPUTY')",
                           (uid,)).fetchone()[0]
        mgmt = dbc.execute("SELECT COUNT(*) FROM memberships m JOIN departments d ON d.id=m.dept_id WHERE m.user_id=? "
                           "AND m.active=1 AND d.is_management=1", (uid,)).fetchone()[0]
        return own > 0 or lead > 0 or mgmt > 0
    check("digest goes only to active people with something to act on", all(has_reason(m[0]) for m in mails),
          [m[0] for m in mails if not has_reason(m[0])])
    check("one digest per person", len({m[0] for m in mails}) == len(mails))
    idle = dbc.execute("SELECT u.email FROM users u WHERE u.status='ACTIVE' AND NOT EXISTS (SELECT 1 FROM tasks t WHERE "
                       "t.assignee_id=u.id AND t.status IN ('TODO','IN_PROGRESS','ON_HOLD') AND t.due_date<>'' AND t.due_date<=?) "
                       "AND NOT EXISTS (SELECT 1 FROM memberships m WHERE m.user_id=u.id AND m.active=1 AND m.role IN ('HEAD','DEPUTY')) "
                       "AND NOT EXISTS (SELECT 1 FROM memberships m JOIN departments d ON d.id=m.dept_id WHERE m.user_id=u.id AND m.active=1 "
                       "AND d.is_management=1)", (tomorrow,)).fetchall()
    check("people with nothing due get no digest", len(idle) > 0 and not ({i[0] for i in idle} & {m[0] for m in mails}))
    check("digest lists overdue work by task key", any("Overdue" in m[2] and "-" in m[2] for m in mails))
    dbc.close()
finally:
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()
    CONSOLE.close()
    out = open(os.path.join(WORK, "console.txt"), errors="replace").read()
    if "!" in out:
        print("\n-- server log lines with '!':")
        for line in out.splitlines():
            if "  ! " in line:
                print(line)
    shutil.rmtree(WORK, ignore_errors=True)

print(f"\n{PASS} passed, {FAIL} failed")
for f in FAILS:
    print("  FAILED:", f)
sys.exit(1 if FAIL else 0)
