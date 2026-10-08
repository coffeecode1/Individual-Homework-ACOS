#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>

#include "config.h"

const char* DIR_FROM[DIR_COUNT]    = {"с севера", "с востока", "с юга", "с запада"};
const char* TURN_NAMES[TURN_COUNT] = {"налево", "прямо", "направо"};

// Имена потоков для вывода и их коды в конфиг-файле (NL = север-налево и т.д.).
// Порядок совпадает с номером потока: направление * 3 + манёвр.
static const char* FLOW_NAMES[FLOW_COUNT] = {
    "С-налево", "С-прямо", "С-направо",
    "В-налево", "В-прямо", "В-направо",
    "Ю-налево", "Ю-прямо", "Ю-направо",
    "З-налево", "З-прямо", "З-направо"
};
static const char* FLOW_CODES[FLOW_COUNT] = {
    "NL", "NS", "NR",  "EL", "ES", "ER",  "SL", "SS", "SR",  "WL", "WS", "WR"
};
static const char TURN_LETTERS[] = "LSR"; // буквы манёвров в строке maneuvers

#define CONFIG_BUF_SIZE 16384 // максимальный размер конфигурационного файла

// Имя потока вида "С-налево"
const char* FlowName(int flow) {
    return FLOW_NAMES[flow];
}

// Номер потока по коду из конфиг-файла ("NS" -> 1) или -1, если код неизвестен
static int FlowByCode(const char* code) {
    for(int i = 0; i < FLOW_COUNT; ++i)
        if(strcmp(code, FLOW_CODES[i]) == 0) return i;
    return -1;
}

// Куда выезжает поток: налево -> следующее направление по часовой,
// прямо -> противоположное, направо -> предыдущее.
static int FlowExit(int flow) {
    return (FlowDir(flow) + 1 + FlowTurn(flow)) % DIR_COUNT;
}

// Матрица конфликтов по умолчанию для правостороннего движения.
// Два потока конфликтуют, если их траектории пересекаются или
// они выезжают на одну и ту же улицу.
static int DefaultConflict(int a, int b) {
    int da = FlowDir(a), db = FlowDir(b);
    int ta = FlowTurn(a), tb = FlowTurn(b);
    if(da == db) return 0; // с одного подхода - не мешают
    if(FlowExit(a) == FlowExit(b)) return 1; // выезжают в одну сторону
    if(ta == TURN_RIGHT || tb == TURN_RIGHT) return 0; // правый поворот никого не пересекает
    int opposite = ((da + 2) % DIR_COUNT == db);
    if(ta == tb) return !opposite; // прямо-прямо и налево-налево: встречные не мешают
    return 1; // прямо и налево всегда пересекаются
}

//------------------------------------------------------------------------------
// Добавление фазы. flows - коды потоков через пробел ("NS NR SS SR") или "ped"
static int AddPhase(Config* cfg, const char* name, const char* flows) {
    if(cfg->phaseCount >= MAX_PHASES) {
        dprintf(2, "Ошибка: слишком много фаз (максимум %d)\n", MAX_PHASES);
        return -1;
    }
    Phase* ph = &cfg->phases[cfg->phaseCount];
    memset(ph, 0, sizeof(*ph));
    snprintf(ph->name, sizeof(ph->name), "%s", name);

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", flows);
    for(char* tok = strtok(tmp, " \t"); tok != NULL; tok = strtok(NULL, " \t")) {
        if(strcmp(tok, "ped") == 0) { ph->isPedestrian = 1; continue; }
        int f = FlowByCode(tok);
        if(f < 0) {
            dprintf(2, "Ошибка: неизвестный поток '%s' в фазе %s\n", tok, name);
            return -1;
        }
        ph->flows[f] = 1;
    }
    cfg->phaseCount++;
    return 0;
}

// Параметры по умолчанию
void ConfigDefaults(Config* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    for(int i = 0; i < FLOW_COUNT; ++i) {
        cfg->allowed[i] = 1;
        for(int j = 0; j < FLOW_COUNT; ++j)
            cfg->conflict[i][j] = DefaultConflict(i, j);
    }
    cfg->carRate[DIR_N] = 30;
    cfg->carRate[DIR_E] = 25;
    cfg->carRate[DIR_S] = 30;
    cfg->carRate[DIR_W] = 25;
    cfg->pedRate = 4;
    cfg->emergencyRate = 2;
    cfg->emergencyPriority = 1;
    cfg->queueCapacity = 10;
    cfg->minGreen = 5;
    cfg->maxGreen = 20;
    cfg->clearance = 3;
    cfg->starvationLimit = 90;
    cfg->strategy = STRATEGY_ADAPTIVE;
    cfg->crossTime = 2;
    cfg->duration = 200;
    cfg->participants = 0;
    cfg->infinite = 0;
    cfg->seed = (unsigned)time(NULL);
    cfg->delayMs = 1000;
    cfg->visual = 1;
    snprintf(cfg->logFile, PATH_LEN, "crossroad.log");
    // Стандартный набор фаз: север-юг, левые с севера и юга,
    // восток-запад, левые с востока и запада, пешеходная фаза.
    AddPhase(cfg, "A", "NS NR SS SR");
    AddPhase(cfg, "B", "NL SL");
    AddPhase(cfg, "C", "ES ER WS WR");
    AddPhase(cfg, "D", "EL WL");
    AddPhase(cfg, "P", "ped");
}

// Чтение файла целиком в буфер с помощью системных вызовов.
// Возвращает число прочитанных байт или -1.
static int ReadWholeFile(const char* fileName, char* buf, int bufSize) {
    int fd = open(fileName, O_RDONLY);
    if(fd == -1) {
        dprintf(2, "Ошибка: не удалось открыть файл '%s'\n", fileName);
        return -1;
    }
    int total = 0;
    ssize_t n;
    while(total < bufSize - 1 && (n = read(fd, buf + total, bufSize - 1 - total)) > 0)
        total += (int)n;
    buf[total] = '\0';
    close(fd);
    return total;
}

// Чтение матрицы
static int LoadConflicts(Config* cfg, const char* fileName) {
    char buf[CONFIG_BUF_SIZE];
    if(ReadWholeFile(fileName, buf, sizeof(buf)) < 0) return -1;
    int k = 0, comment = 0;
    for(char* p = buf; *p; ++p) {
        if(*p == '#') comment = 1;
        if(*p == '\n') comment = 0;
        if(comment || (*p != '0' && *p != '1')) continue;
        if(k < FLOW_COUNT * FLOW_COUNT)
            cfg->conflict[k / FLOW_COUNT][k % FLOW_COUNT] = *p - '0';
        ++k;
    }
    if(k != FLOW_COUNT * FLOW_COUNT) {
        dprintf(2, "Ошибка: в файле '%s' должно быть 144 числа 0/1 (матрица 12x12)\n", fileName);
        return -1;
    }
    return 0;
}

// Удаление пробелов в начале и конце строки
static char* Trim(char* s) {
    while(*s == ' ' || *s == '\t') ++s;
    char* e = s + strlen(s);
    while(e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) --e;
    *e = '\0';
    return s;
}

// Применение одной строки "ключ = значение" конфигурационного файла
static int ApplyOption(Config* cfg, const char* key, char* value, int* phasesReset) {
    if(strcmp(key, "strategy") == 0) {
        if(strcmp(value, "fixed") == 0) cfg->strategy = STRATEGY_FIXED;
        else if(strcmp(value, "adaptive") == 0) cfg->strategy = STRATEGY_ADAPTIVE;
        else return -1;
    }
    else if(strcmp(key, "car_rate") == 0) {
        if(sscanf(value, "%d %d %d %d", &cfg->carRate[0], &cfg->carRate[1],
                  &cfg->carRate[2], &cfg->carRate[3]) != 4) return -1;
    }
    else if(strcmp(key, "maneuvers") == 0) {
        // 4 слова для С, В, Ю, З: буквы L, S, R или '-' (нет манёвров)
        char* tok = strtok(value, " \t");
        for(int d = 0; d < DIR_COUNT; ++d) {
            if(tok == NULL) return -1;
            for(int t = 0; t < TURN_COUNT; ++t)
                cfg->allowed[FlowIndex(d, t)] = (strchr(tok, TURN_LETTERS[t]) != NULL);
            tok = strtok(NULL, " \t");
        }
    }
    else if(strcmp(key, "phase") == 0) {
        // phase = ИМЯ поток поток ...
        if(!*phasesReset) { cfg->phaseCount = 0; *phasesReset = 1; }
        char name[16];
        int n = 0;
        if(sscanf(value, "%15s %n", name, &n) != 1) return -1;
        if(AddPhase(cfg, name, value + n) != 0) return -1;
    }
    else if(strcmp(key, "conflicts") == 0) {
        if(LoadConflicts(cfg, value) != 0) return -1;
    }
    else if(strcmp(key, "log") == 0) {
        snprintf(cfg->logFile, PATH_LEN, "%s", value);
    }
    else {
        // Остальные параметры - целые числа
        struct { const char* name; int* field; } ints[] = {
            {"ped_rate", &cfg->pedRate},
            {"emergency_rate", &cfg->emergencyRate},
            {"emergency_priority", &cfg->emergencyPriority},
            {"queue_capacity", &cfg->queueCapacity},
            {"min_green", &cfg->minGreen},
            {"max_green", &cfg->maxGreen},
            {"clearance", &cfg->clearance},
            {"starvation_limit", &cfg->starvationLimit},
            {"cross_time", &cfg->crossTime},
            {"duration", &cfg->duration},
            {"participants", &cfg->participants},
            {"infinite", &cfg->infinite},
            {"delay", &cfg->delayMs},
            {"visual", &cfg->visual},
        };
        for(size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); ++i) {
            if(strcmp(key, ints[i].name) == 0)
                return (sscanf(value, "%d", ints[i].field) == 1) ? 0 : -1;
        }
        if(strcmp(key, "seed") == 0)
            return (sscanf(value, "%u", &cfg->seed) == 1) ? 0 : -1;
        dprintf(2, "Ошибка: неизвестный параметр '%s'\n", key);
        return -1;
    }
    return 0;
}

// Чтение конфигурационного файла формата "ключ = значение", '#' - комментарий
int ConfigLoad(Config* cfg, const char* fileName) {
    char buf[CONFIG_BUF_SIZE];
    if(ReadWholeFile(fileName, buf, sizeof(buf)) < 0) return -1;
    int phasesReset = 0; // при первой строке phase стандартные фазы удаляются
    int lineNo = 0;
    // разбираем буфер построчно (строки разделены '\n')
    for(char* line = buf; line != NULL; ) {
        char* next = strchr(line, '\n');
        if(next) *next++ = '\0';
        ++lineNo;
        char* hash = strchr(line, '#');
        if(hash) *hash = '\0';
        char* eq = strchr(line, '=');
        if(eq != NULL) {
            *eq = '\0';
            char* key = Trim(line);
            char* value = Trim(eq + 1);
            if(ApplyOption(cfg, key, value, &phasesReset) != 0) {
                dprintf(2, "Ошибка в файле '%s', строка %d (параметр '%s')\n",
                        fileName, lineNo, key);
                return -1;
            }
        } else if(*Trim(line) != '\0') {
            dprintf(2, "Ошибка в файле '%s', строка %d: ожидается 'ключ = значение'\n",
                    fileName, lineNo);
            return -1;
        }
        line = next;
    }
    return 0;
}

// Справка по аргументам командной строки
static void PrintUsage(const char* prog) {
    dprintf(1,
        "Использование: %s [опции]\n"
        "  -c, --config FILE   конфигурационный файл\n"
        "  -m, --mode MODE     стратегия: fixed | adaptive\n"
        "  -t, --time N        период наблюдения, тактов (0 - без ограничения)\n"
        "  -n, --count N       завершить после пропуска N участников\n"
        "  -i, --infinite      работать до Ctrl+C\n"
        "  -s, --seed N        начальное значение генератора случайных чисел\n"
        "  -d, --delay MS      задержка между тактами, мс\n"
        "  -q, --quiet         не рисовать перекрёсток, выводить только события\n"
        "  -l, --log FILE      файл журнала (по умолчанию crossroad.log)\n"
        "  -h, --help          эта справка\n", prog);
}

// Разбор аргументов командной строки.
// Сначала ищем -c и читаем конфиг-файл, потом остальные опции
// записываются прямо в cfg и перекрывают значения из файла.
int ConfigParseArgs(Config* cfg, int argc, char** argv) {
    static struct option longOpts[] = {
        {"config",   required_argument, 0, 'c'},
        {"mode",     required_argument, 0, 'm'},
        {"time",     required_argument, 0, 't'},
        {"count",    required_argument, 0, 'n'},
        {"infinite", no_argument,       0, 'i'},
        {"seed",     required_argument, 0, 's'},
        {"delay",    required_argument, 0, 'd'},
        {"quiet",    no_argument,       0, 'q'},
        {"log",      required_argument, 0, 'l'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    ConfigDefaults(cfg);

    // 1) конфиг-файл (если указан)
    for(int i = 1; i + 1 < argc; ++i)
        if(strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0)
            if(ConfigLoad(cfg, argv[i + 1]) != 0) return -1;

    // 2) остальные опции
    int opt;
    while((opt = getopt_long(argc, argv, "c:m:t:n:is:d:ql:h", longOpts, NULL)) != -1) {
        switch(opt) {
        case 'c': break; // уже прочитан выше
        case 'm':
            if(strcmp(optarg, "fixed") == 0) cfg->strategy = STRATEGY_FIXED;
            else if(strcmp(optarg, "adaptive") == 0) cfg->strategy = STRATEGY_ADAPTIVE;
            else { dprintf(2, "Ошибка: неизвестная стратегия '%s'\n", optarg); return -1; }
            break;
        case 't': cfg->duration = atoi(optarg); break;
        case 'n': cfg->participants = atoi(optarg); break;
        case 'i': cfg->infinite = 1; break;
        case 's': cfg->seed = (unsigned)atol(optarg); break;
        case 'd': cfg->delayMs = atoi(optarg); break;
        case 'q': cfg->visual = 0; break;
        case 'l': snprintf(cfg->logFile, PATH_LEN, "%s", optarg); break;
        case 'h': PrintUsage(argv[0]); return 1;
        default:  PrintUsage(argv[0]); return -1;
        }
    }
    return 0;
}

// Сообщение об ошибке параметров. Всегда возвращает -1.
static int Fail(const char* msg) {
    dprintf(2, "Ошибка: %s\n", msg);
    return -1;
}

// Проверка параметров. При первой найденной ошибке выводит её и возвращает -1.
int ConfigValidate(const Config* cfg) {
    char msg[200];
    if(cfg->queueCapacity < 1 || cfg->queueCapacity > MAX_QUEUE)
        return Fail("вместимость очереди должна быть от 1 до 100");
    if(cfg->minGreen < 1 || cfg->maxGreen < cfg->minGreen)
        return Fail("нужно 1 <= min_green <= max_green");
    if(cfg->clearance < 1 || cfg->crossTime < 1)
        return Fail("clearance и cross_time должны быть >= 1");
    if(cfg->starvationLimit < cfg->maxGreen)
        return Fail("starvation_limit должен быть >= max_green");
    if(cfg->phaseCount < 1)
        return Fail("должна быть хотя бы одна фаза");

    // Матрица конфликтов должна быть симметричной
    for(int i = 0; i < FLOW_COUNT; ++i)
        for(int j = 0; j < FLOW_COUNT; ++j)
            if(cfg->conflict[i][j] != cfg->conflict[j][i]) {
                snprintf(msg, sizeof(msg), "матрица конфликтов несимметрична (%s / %s)",
                         FlowName(i), FlowName(j));
                return Fail(msg);
            }

    // Инвариант 1: внутри одной фазы нет конфликтующих потоков
    int hasPedPhase = 0;
    for(int p = 0; p < cfg->phaseCount; ++p) {
        const Phase* ph = &cfg->phases[p];
        if(ph->isPedestrian) hasPedPhase = 1;
        for(int i = 0; i < FLOW_COUNT; ++i)
            for(int j = i + 1; j < FLOW_COUNT; ++j)
                if(ph->flows[i] && ph->flows[j] && cfg->conflict[i][j]) {
                    snprintf(msg, sizeof(msg), "фаза %s содержит конфликтующие потоки %s и %s",
                             ph->name, FlowName(i), FlowName(j));
                    return Fail(msg);
                }
    }
    if(cfg->pedRate > 0 && !hasPedPhase)
        return Fail("есть пешеходы, но нет пешеходной фазы");

    // Каждый разрешённый манёвр должен входить хотя бы в одну фазу
    for(int i = 0; i < FLOW_COUNT; ++i) {
        if(!cfg->allowed[i]) continue;
        int covered = 0;
        for(int p = 0; p < cfg->phaseCount; ++p)
            if(cfg->phases[p].flows[i]) covered = 1;
        if(!covered) {
            snprintf(msg, sizeof(msg), "поток %s не входит ни в одну фазу", FlowName(i));
            return Fail(msg);
        }
    }
    return 0;
}

// Вывод основных параметров модели в дескриптор fd
void ConfigPrint(const Config* cfg, int fd) {
    dprintf(fd, "Параметры модели:\n");
    dprintf(fd, "  стратегия: %s, фаза %d..%d тактов, переходный интервал %d\n",
            cfg->strategy == STRATEGY_FIXED ? "фиксированный цикл" : "адаптивная",
            cfg->minGreen, cfg->maxGreen, cfg->clearance);
    dprintf(fd, "  машины С/В/Ю/З: %d/%d/%d/%d %%, пешеходы: %d %%, экстренные: %d %%\n",
            cfg->carRate[0], cfg->carRate[1], cfg->carRate[2], cfg->carRate[3],
            cfg->pedRate, cfg->emergencyRate);
    dprintf(fd, "  очередь: %d мест, проезд: %d такта, фаз: %d\n",
            cfg->queueCapacity, cfg->crossTime, cfg->phaseCount);
    dprintf(fd, "  seed = %u, задержка %d мс\n", cfg->seed, cfg->delayMs);
}
