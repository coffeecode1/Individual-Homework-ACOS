#ifndef CONFIG_H
#define CONFIG_H

//  Параметры модели перекрёстка.
//  Параметры могут читаться из командной строки

#define DIR_COUNT 4 // север, восток, юг, запад
#define TURN_COUNT 3 // налево, прямо, направо
#define FLOW_COUNT 12 // всего автомобильных полос
#define WALK_COUNT 4 // пешеходные переходы

#define MAX_PHASES 8 // максимальное число фаз светофора
#define MAX_QUEUE 100 // максимальная вместимость одной очереди
#define PATH_LEN 256 // максимальная длина пути к файлу

// Направления (откуда приехал автомобиль).
enum { DIR_N = 0, DIR_E = 1, DIR_S = 2, DIR_W = 3 };
// Повороты
enum { TURN_LEFT = 0, TURN_STRAIGHT = 1, TURN_RIGHT = 2 };

// Стратегия управления светофором
typedef enum Strategy {
    STRATEGY_FIXED, // фиксированный цикл: фазы по кругу, каждая max_green тактов
    STRATEGY_ADAPTIVE // адаптивная: учитывает длины очередей и время ожидания
} Strategy;

// Фаза светофора - множество одновременно разрешённых (совместимых) потоков
typedef struct Phase {
    char name[16]; // имя фазы (A, B, ...)
    int  isPedestrian; // 1 - пешеходная фаза (всем машинам красный)
    int  flows[FLOW_COUNT]; // flows[i] == 1 - потоку i горит зелёный
} Phase;

// Все параметры модели
typedef struct Config {
    int allowed[FLOW_COUNT]; // разрешённые полосы
    int conflict[FLOW_COUNT][FLOW_COUNT]; // матрица конфликтов

    // интенсивности
    int carRate[DIR_COUNT]; // автомобили по каждому подходу
    int pedRate; // пешеходы на каждом переходе
    int emergencyRate; // доля экстренного транспорта среди машин, %
    int emergencyPriority; // 1 - экстренному транспорту даётся приоритет

    // очереди
    int queueCapacity; // вместимость очереди (у полосы и у перехода)

    // светофор
    int minGreen; // минимальная длительность фазы, кол-во тактов
    int maxGreen; // максимальная длительность фазы, кол-во тактов
    int clearance; // переходный интервал (жёлтый + все красные)
    int starvationLimit; // если кто-то ждёт дольше - его фаза включается первой
    Strategy strategy; // стратегия управления
    int phaseCount; // число фаз
    Phase phases[MAX_PHASES]; // упорядоченные фазы цикла

    // движение через зону
    int crossTime; // за сколько тактов участник пересекает перекрёсток

    // режим работы
    int duration; // период наблюдения, кол-во тактов
    int participants; // завершить после пропуска стольких участников (0 - нет)
    int infinite; // 1 - работать до прерывания пользователем
    unsigned seed; // начальное значение генератора случайных чисел
    int delayMs; // задержка между тактами, мс
    int visual; // 1 - рисовать перекрёсток, 0 - только текст событий
    char logFile[PATH_LEN]; // имя файла журнала
} Config;

// Имена направлений и сторон для вывода
extern const char* DIR_FROM[DIR_COUNT]; // "с севера", ...
extern const char* TURN_NAMES[TURN_COUNT]; // "налево", ...

// Номер полосы на всем перекрестке
static inline int FlowIndex(int dir, int turn) { return dir * TURN_COUNT + turn; }
static inline int FlowDir(int flow)  { return flow / TURN_COUNT; }
static inline int FlowTurn(int flow) { return flow % TURN_COUNT; }

// Имя потока вида "С-налево" (возвращает указатель на статический буфер)
const char* FlowName(int flow);

// Заполнение параметров значениями по умолчанию
void ConfigDefaults(Config* cfg);

// Чтение конфигурационного файла. Возвращает 0 при успехе.
int ConfigLoad(Config* cfg, const char* fileName);

// Разбор аргументов командной строки. Возвращает 0 - продолжать,
// 1 - выйти без ошибки (была справка), -1 - ошибка.
int ConfigParseArgs(Config* cfg, int argc, char** argv);

// Проверка корректности параметров (в т.ч. что фазы не содержат
// конфликтующих потоков). Возвращает 0, если всё верно.
int ConfigValidate(const Config* cfg);

// Вывод параметров модели в файл с дескриптором fd
void ConfigPrint(const Config* cfg, int fd);

#endif // CONFIG_H
