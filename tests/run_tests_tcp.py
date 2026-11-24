#!/usr/bin/env python3
# Запуск из проекта: python3 tests/run_tests_tcp.py
import os
from pathlib import Path
import signal
import select
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
OUT = Path(tempfile.mkdtemp(prefix="crossroad-tcp-"))
PROCESSES = []


def start(name, args):
    path = OUT / (name + ".txt")
    with path.open("w") as output:
        process = subprocess.Popen(args, stdout=output, stderr=subprocess.STDOUT)
    PROCESSES.append(process)
    return process, path


def wait_for(path, text, process, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if path.exists() and text in path.read_text():
            return
        if process.poll() is not None:
            raise AssertionError(path.read_text())
        time.sleep(0.02)
    raise AssertionError(f"Не дождались {text} в {path}")


def server(name, args):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = str(probe.getsockname()[1])
    log = OUT / (name + ".log")
    process, output = start(name, [
        "./bin/crossroad-server", "127.0.0.1", port, "-q", "-d", "0", "-s", "39",
        *args, "-l", str(log),
    ])
    wait_for(output, "TCP READY", process)
    return process, output, log, port


def clients(name, port):
    return [start(name + "-" + side, [
        "./bin/crossroad-client", "127.0.0.1", port, side,
    ])[0] for side in ("north", "east", "south", "west")]


def observer(name, port):
    return start(name, ["./bin/crossroad-observer", "127.0.0.1", port])


def finish(process, workers, log, reason):
    assert process.wait(timeout=30) == 0, log.read_text()
    for worker in workers:
        assert worker.wait(timeout=5) == 0
    text = log.read_text()
    assert reason in text and "Нарушений инвариантов: 0" in text
    return text


def main():
    print(f"Журналы {OUT}", flush=True)
    cases = [
        ("base", "истёк период наблюдения"),
        ("lanes", "пропущено заданное число участников"),
        ("overload", "ОТКАЗ"),
        ("stress", "истёк период наблюдения"),
    ]
    for name, reason in cases:
        process, output, log, port = server(name, ["-c", f"data/{name}.cfg", "-d", "0"])
        watch, watched = observer(name + "-observer", port)
        wait_for(watched, "СОСТОЯНИЕ", watch)
        workers = clients(name, port)
        finish(process, workers, log, reason)
        assert watch.wait(timeout=5) == 0
        text = watched.read_text()
        assert "END " in text and "нарушения 0" in text
        # Наблюдатель получает те же события, что и журнал.
        events = [line for line in log.read_text().splitlines() if line.startswith("[t=") and "] СЕТЬ:" not in line]
        displayed = [line for line in text.splitlines() if line.startswith("[t=") and "] СЕТЬ:" not in line]
        assert all(event in displayed for event in events[2:])
        print(f"OK {name}", flush=True)

    process, _, log, port = server("repeat", ["-c", "data/base.cfg", "-d", "0"])
    finish(process, clients("repeat", port), log, "истёк период наблюдения")
    before = [line for line in (OUT / "base.log").read_text().splitlines() if line.startswith("[t=") and "] СЕТЬ:" not in line]
    after = [line for line in log.read_text().splitlines() if line.startswith("[t=") and "] СЕТЬ:" not in line]
    assert before == after
    print("OK одинаковый seed с наблюдателем и без", flush=True)

    process, output, log, port = server("dynamic", ["-i", "-d", "15"])
    first, first_log = observer("watch-1", port)
    second, second_log = observer("watch-2", port)
    wait_for(first_log, "СОСТОЯНИЕ", first)
    wait_for(second_log, "СОСТОЯНИЕ", second)
    workers = clients("dynamic", port)
    wait_for(second_log, "СОСТОЯНИЕ t=5,", second)
    first.terminate()
    first.wait(timeout=5)

    # Отдельно проверяем, что занятое направление не получает второй клиент.
    duplicate, _ = start("duplicate", [
        "./bin/crossroad-client", "127.0.0.1", port, "north",
    ])
    assert duplicate.wait(timeout=5) != 0
    wait_for(second_log, "СОСТОЯНИЕ t=8,", second)

    # Медленное приветствие и несколько команд в одном TCP-пакете.
    with socket.create_connection(("127.0.0.1", int(port)), timeout=3) as raw:
        for part in (b"W", b"AT", b"CH\n"):
            raw.sendall(part)
        assert "СОСТОЯНИЕ" in raw.recv(4096).decode()
    with socket.create_connection(("127.0.0.1", int(port)), timeout=3) as raw:
        raw.sendall(b"BAD\nBAD\n")
        assert raw.recv(4096).startswith(b"ERROR ")

    # Сервер продолжает работать и совсем без наблюдателей.
    second.terminate()
    second.wait(timeout=5)
    wait_for(log, "[t=0012]", process)
    third, third_log = observer("watch-reconnected", port)
    wait_for(third_log, "СОСТОЯНИЕ", third)
    assert "СОСТОЯНИЕ t=0," not in third_log.read_text()
    process.send_signal(signal.SIGINT)
    finish(process, workers, log, "прервано пользователем")
    assert third.wait(timeout=5) == 0
    print("OK несколько наблюдателей, отключение, повторное подключение, Ctrl+C", flush=True)

    process, output, log, port = server("reconnect", ["-c", "data/base.cfg", "-d", "0"])
    watch, watched = observer("reconnect-observer", port)
    wait_for(watched, "СОСТОЯНИЕ", watch)
    # Обрываем ответ на середине строки. Такой ответ применять нельзя.
    raw = socket.create_connection(("127.0.0.1", int(port)), timeout=3)
    reader = raw.makefile("rb")
    raw.sendall(b"WORK 0\n")
    assert reader.readline().startswith(b"CONFIG ")
    workers = [start("reconnect-" + side, [
        "./bin/crossroad-client", "127.0.0.1", port, side,
    ])[0] for side in ("east", "south", "west")]
    assert reader.readline() == b"TICK 0\n"
    raw.sendall(b"ARRIVE 0 -1 0")
    raw.shutdown(socket.SHUT_WR)
    assert reader.readline() == b""
    reader.close()
    raw.close()
    wait_for(log, "сторона 0 отключилась", process)
    time.sleep(5.2)  # дольше прежнего таймаута ответа
    assert process.poll() is None
    assert all(worker.poll() is None for worker in workers)
    assert "СОСТОЯНИЕ t=1," not in watched.read_text()
    replacement, _ = start("north-restarted", [
        "./bin/crossroad-client", "127.0.0.1", port, "north",
    ])
    finish(process, workers + [replacement], log, "истёк период наблюдения")
    assert watch.wait(timeout=5) == 0
    resumed = [line for line in log.read_text().splitlines()
               if line.startswith("[t=") and "] СЕТЬ:" not in line]
    assert resumed == before
    print("OK повторный запуск после неполного ответа, события совпали", flush=True)

    # Полученный ответ тоже сохраняем, даже если клиент сразу отключился.
    process, _, log, port = server("saved-answer", ["-t", "1"])
    connections = []
    readers = []
    for direction in (0, 3):
        connection = socket.create_connection(("127.0.0.1", int(port)), timeout=3)
        connection.sendall(f"WORK {direction}\n".encode())
        stream = connection.makefile("rb")
        assert stream.readline().startswith(b"CONFIG ")
        connections.append(connection)
        readers.append(stream)
    workers = [start("saved-" + side, [
        "./bin/crossroad-client", "127.0.0.1", port, side,
    ])[0] for side in ("east", "south")]
    assert readers[0].readline() == b"TICK 0\n"
    assert readers[1].readline() == b"TICK 0\n"
    connections[0].sendall(b"ARRIVE 0 -1 0 1\n")
    connections[0].shutdown(socket.SHUT_WR)
    assert readers[0].readline() == b""
    readers[0].close()
    connections[0].close()
    replacement = socket.create_connection(("127.0.0.1", int(port)), timeout=3)
    replacement.sendall(b"WORK 0\n")
    replacement_reader = replacement.makefile("rb")
    assert replacement_reader.readline().startswith(b"CONFIG ")
    assert not select.select([replacement], [], [], 0.2)[0]
    connections[1].sendall(b"ARRIVE 0 -1 0 0\n")
    finish(process, workers, log, "истёк период наблюдения")
    assert b"END " in replacement_reader.read()
    assert b"END " in readers[1].read()
    replacement_reader.close()
    replacement.close()
    readers[1].close()
    connections[1].close()
    assert log.read_text().count("к северному переходу") == 1
    print("OK сохранение уже принятого ответа без повторного применения", flush=True)

    process, output, log, port = server("paused-stop", ["-i", "-d", "10"])
    workers = clients("paused", port)
    wait_for(log, "[t=0005]", process)
    # Несколько клиентов можно перезапустить, а сервер остановить и во время паузы.
    for i, side in enumerate(("north", "east")):
        workers[i].terminate()
        workers[i].wait(timeout=5)
        wait_for(log, f"сторона {i} отключилась", process)
        workers[i], _ = start("paused-restarted-" + side, [
            "./bin/crossroad-client", "127.0.0.1", port, side,
        ])
    wait_for(log, "[t=0010]", process)
    workers[0].terminate()
    workers[0].wait(timeout=5)
    wait_for(log, "сторона 0 отключилась", process)
    process.send_signal(signal.SIGTERM)
    finish(process, workers[1:], log, "прервано пользователем")
    print("OK перезапуск разных сторон и остановка сервера во время паузы", flush=True)

    for args in (
        ["127.0.0.1", "0"],
        ["не-IP", "12345"],
        ["127.0.0.1", "12345", "-c", "data/bad-phase.cfg"],
        ["127.0.0.1", "12345", "-c", "data/no-file.cfg"],
    ):
        process, _ = start("invalid", ["./bin/crossroad-server", *args])
        assert process.wait(timeout=5) != 0
    print("OK неверные параметры", flush=True)


try:
    main()
finally:
    for process in PROCESSES:
        if process.poll() is None:
            process.terminate()
    for process in PROCESSES:
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
