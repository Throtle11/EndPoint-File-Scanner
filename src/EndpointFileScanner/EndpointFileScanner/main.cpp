// EndpointFileScanner.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
// 
//문자깨짐현상으로 인한 추가 
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
//윈도우가 아니면 컴파일되지않음

#include <iostream>
#include <string>
#include <filesystem>
#include <system_error>
#include <vector>
#include <cctype>
#include <algorithm>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <utility>


namespace fs = std::filesystem;
using std::cout;
using std::endl;


//============================= 데이터 구조체 =============================

//파일의 상세정보 기록
struct FileEntry
{
    fs::path path; //파일 전체경로 저장
    std::uintmax_t size = 0; //파일의 크기를 바이트 단위로 저장
    std::string ext;//파일의 확장자를 문자열로 저장
    fs::file_time_type mtime;//파일의 마지막 수정 시간 저장
};

//파일의 전체합계/통계 저장
struct Stats
{
    std::size_t total = 0; //총 스캔항목수
    std::size_t files = 0; //파일로 판정된 항목수
    std::size_t dirs = 0;  //폴더로 판정된 항목수
    std::size_t skipped = 0;//제외된 항목수(오류/필터 제외 포함)

    std::size_t data_ok = 0;//데이터수집이 완료된 항목수
    std::size_t data_fail = 0;//데이터수집에 실패한항목수
};

// 실행 옵션(스캔 경로, CSV 저장 경로)을 담는 구조체
struct CliOptions
{
    std::string scanPath; // 스캔 대상 폴더
    std::string outPath;  // --out으로 받은 CSV 경로(없으면 빈 문자열)
};

//파일 필터링 조건 구조체 
struct FilterConfig
{
    std::uintmax_t minSize = 0;              // 최소 크기
    std::uintmax_t maxSize = 0;              // 최대 크기

    std::vector<std::string> includeExts;    // 포함 확장자 목록(비어있다면 모두허용)
    std::vector<std::string> excludeExts;    // 제외 확장자 목록(비어있다면 모두허용)
};


//============================= 범용 유틸 =============================

//문자열을 소문자로 변환
static std::string ToLower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

//=============================입력 1줄 토큰화 =============================
// 예: "C:\Program Files" --out "report.csv"
static std::vector<std::string> TokenizeCommandLine(const std::string& line)
{
    std::vector<std::string> tokens; //토큰을 저장할 배열
    std::string cur; //현재 처리중인 토근을 담아둠
    bool inQuotes = false;

    for (char c : line)
    {
        if (c == '"') { inQuotes = !inQuotes; continue; }

        if (!inQuotes && std::isspace((unsigned char)c))
        {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            continue;
        }

        cur.push_back(c);
    }

    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

//확장자가 확장자 목록에 있는지 검사
static bool InListInsensitive(const std::string& val, const std::vector<std::string>& list)
{
    for (const auto& x : list)  
        if (ToLower(val) == ToLower(x))
            return true;
    return false;
}


//============================= RAII 스코프가드 =============================

template <class F>
class ScopeGuard
{
private:
    F func_;
    bool active_;
public:
    explicit ScopeGuard(F f) : func_(std::move(f)), active_(true) {}
    ~ScopeGuard() { if (active_) func_(); }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

    ScopeGuard(ScopeGuard&& other) noexcept
        : func_(std::move(other.func_)), active_(other.active_)
    {
        other.active_ = false;
    }

    void Dismiss() { active_ = false; }
};

template <class F>
static ScopeGuard<F> MakeScopeGuard(F f)
{
    return ScopeGuard<F>(std::move(f));
}


//============================= 로그출력 =============================

static std::ofstream g_log;

static std::string NowString()
{
    using namespace std::chrono;

    auto now = system_clock::now();
    std::time_t tt = system_clock::to_time_t(now);

    std::tm time{};
#ifdef _WIN32
    //윈도우
    localtime_s(&time, &tt);
#else
    //유닉스,리눅스
    localtime_r(&time, &tt);
#endif

    std::ostringstream oss;
    oss << std::put_time(&time, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

//콘솔,파일에 로그출력
static void Log(const std::string& level, const std::string& msg)
{
    std::string line = "[" + NowString() + "][" + level + "] " + msg;

    // 콘솔
    cout << line << endl;

    // 파일
    if (g_log.is_open())
        g_log << line << endl;
}


//============================= 문자깨짐 현상 출력지원 =============================

//문자깨짐현상으로 인한 UTF-8 콘솔 출력 지원(윈도우)
#ifdef _WIN32
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();

    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return std::wstring();

    std::wstring w;
    w.resize(len);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}

//윈도우콘솔에 유니코드 출력
static void ConsoleWriteWide(const std::wstring& w)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr)//핸들 유효성검사
    {
        // 콘솔 핸들이 아니면 fallback
        std::string s(w.begin(), w.end());
        cout << s;
        return;
    }

    //콘솔인지 확인
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode))
    {
		// 콘솔이 아닌 파일/리디렉션이면 fallback
        std::string s(w.begin(), w.end());
        cout << s;
        return;
    }

    //문자열을 콘솔에 출력
    //요청한 글자수와 실제 출력된 글자수가 달랐을경우 에러메시지 출력
    DWORD written = 0;
    if (!WriteConsoleW(h, w.c_str(), (DWORD)w.size(), &written, nullptr)) {
        cout << "[에러] 콘솔 출력을 실패했소" << endl;
    }
    else if (written != (DWORD)w.size()) {
        cout << "일부 글자만 출력되었소" << endl;
    }
}

// 유니코드(UTF-16)를 문자 단위로 축약하여 문자열 손상 방지
static std::wstring TruncateMiddleW(const std::wstring& s, std::size_t maxLen)
{
    if (s.size() <= maxLen) return s;
    if (maxLen <= 3) return s.substr(0, maxLen);

    std::size_t keep = maxLen - 3;
    std::size_t head = keep / 2;
    std::size_t tail = keep - head;

    return s.substr(0, head) + L"..." + s.substr(s.size() - tail);
}
#endif



// 실행 인자에서 스캔 경로와 --out(저장 경로)을 찾아서 저장
static CliOptions ParseArgs(int argc, char* argv[])
{
    CliOptions opt;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];

        // --out <file>
        if (a == "--out")
        {
            if (i + 1 < argc)
            {
                opt.outPath = argv[i + 1];
                ++i; // 다음 인자 소비
            }
            else
            {
                cout << "[오류] --out 뒤에 파일 경로가 필요하외다." << endl;
                Log("[에러]", "--out 뒤에 파일 경로가 없구먼");
            }
            continue;
        }

        //옵션이 아닌 첫 인자를 스캔 경로로 정함
        if (!a.empty() && a[0] != '-' && opt.scanPath.empty())
        {
            opt.scanPath = a;
        }
    }

    return opt;
}

//경로가 인자로 들어오면 사용/없으면 사용자입력
static std::string GetPathFromArgsOrInput(int num, char* values[], CliOptions& opt)
{
    std::string path;

    if (num >= 2)
    {
        path = values[1];
        if (opt.scanPath.empty())
            opt.scanPath = path;
        return path;
    }

    cout << "스캔할 폴더 경로를 입력하시게나:";
    std::string line;
    std::getline(std::cin, line);

    //입력된 경로를 토큰으로 분해
    auto tokens = TokenizeCommandLine(line);
    if (tokens.empty())
        return "";

    // 첫 토큰: 스캔 경로
    path = tokens[0];
    opt.scanPath = path;

    // 나머지 토큰
    for (size_t i = 1; i < tokens.size(); ++i)
    {
        if (tokens[i] == "--out")
        {
            if (i + 1 < tokens.size())
            {
                opt.outPath = tokens[i + 1];
                ++i;
            }
            else
            {
                cout << "[오류] --out 뒤에 파일 경로를 입력하시게나:" << endl;
                Log("[에러]", "--out 뒤에 파일 경로가 없구먼");
            }
        }
    }

    return path;
}

// 경로유효성 검사
static bool ValidatePath(const std::string& path)
{
    std::error_code ec;

    if (path.empty()) {
        cout << "[오류] 경로가 비어있는게로구먼." << endl;
        Log("[에러]", "경로가 비어있구먼");
        return false;
    }

    ec.clear();
    bool exists = fs::exists(path, ec);
    if (ec || !exists) {
        cout << "[오류] 경로 접근/확인 실패이외다: " << path;
        if (ec) cout << " (" << ec.message() << ")";
        cout << endl;

        Log("[에러]", "경로 접근/확인 실패일세: " + path + (ec ? (" (" + ec.message() + ")") : ""));
        return false;
    }

    ec.clear();
    bool isDir = fs::is_directory(path, ec);
    if (ec || !isDir) {
        cout << "[오류] 폴더가 아닌게지라: " << path;
        if (ec) cout << " (" << ec.message() << ")";
        cout << endl;

        Log("[에러]", "폴더 아님/확인 실패일세: " + path + (ec ? (" (" + ec.message() + ")") : ""));
        return false;
    }

    return true;
}

//============================= 스캔 + 데이터 수집 =============================
// 폴더 재귀순회 준비
static std::vector<FileEntry> ScanDirectory(const fs::path& root, Stats& st)
{
    std::vector<FileEntry> entries;
    entries.reserve(2048);

    std::error_code ec;

    ec.clear();
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;

    if (ec) {
        cout << "[오류] 디렉터리 순환 시작 실패일세: " << ec.message() << endl;
        Log("[에러]", "디렉터리 순환 시작 실패일세: " + root.u8string() + " (" + ec.message() + ")");
        return entries;
    }

    for (; it != end; it.increment(ec))
    {
        ++st.total;

        // 순회중 오류가 있다면 스킵하고 계속
        if (ec) {
            ++st.skipped;
            Log("[WARNING]", "순회 중 오류로 스킵하겠소: (" + ec.message() + ")");
            ec.clear();
            continue;
        }

        const fs::directory_entry& entry = *it;

        // 파일 판정은 iec로 분리
        //폴더를 순환하며 파일의 데이터수집,오류는 스킵
        std::error_code iec;
        if (entry.is_regular_file(iec))
        {
            ++st.files;

            FileEntry fe;
            fe.path = entry.path();
            fe.ext = ToLower(fe.path.extension().string());

            // 데이터 수집은 mec로 분리
            std::error_code mec;

            fe.size = entry.file_size(mec);//파일크기
            if (mec) {
                ++st.data_fail;
                ++st.skipped;
                Log("[WARNING]", "file_size 실패로 스킵하겠소: " + fe.path.u8string() + " (" + mec.message() + ")");
                continue;
            }

            fe.mtime = entry.last_write_time(mec);//마지막 수정시간
            if (mec) {
                ++st.data_fail;
                ++st.skipped;
                Log("[WARNING]", "last_write_time 실패로 스킵하겠소: " + fe.path.u8string() + " (" + mec.message() + ")");
                continue;
            }

            ++st.data_ok;
            entries.push_back(std::move(fe));
        }
        // 파일이 아니고, 판정 오류도 아니라면(폴더/링크 등)
        else if (!iec)
        {
            // 폴더 수 집계
            std::error_code dec;
            if (entry.is_directory(dec) && !dec)
                ++st.dirs;
            else if (dec)
            {
                ++st.skipped;
                Log("[WARNING]", "디렉터리 판정 실패로 스킵하겠소: " + entry.path().u8string() + " (" + dec.message() + ")");
            }

            // 폴더가 아니면 통과
        }
        // 판정 자체가 실패(오류)
        else
        {
            // 파일 판정 자체가 실패한 경우 스킵
            ++st.skipped;
            Log("[WARNING]", "정규 파일 판정 실패로 스킵하겠소: " + entry.path().u8string() + " (" + iec.message() + ")");
        }
        // 정규 파일이 아닌 항목(폴더/링크 등)은 통과
    }

    return entries;
}

//============================= 필터링 =============================
static std::vector<FileEntry> FilterEntries(const std::vector<FileEntry>& entries,
    const FilterConfig& cfg,
    Stats& st)
{
    std::vector<FileEntry> out;
    out.reserve(entries.size());//입력으로 받은 개수만큼 메모리확보

    for (const auto& e : entries)
    {
        bool reject = false;
        //크기 필터
        if (cfg.minSize != 0 && e.size < cfg.minSize) reject = true;
        if (cfg.maxSize != 0 && e.size > cfg.maxSize) reject = true;

        //포함확장자/제외확장자
        if (!reject)
        {
            if (!cfg.includeExts.empty() && !InListInsensitive(e.ext, cfg.includeExts))
                reject = true;

            if (!cfg.excludeExts.empty() && InListInsensitive(e.ext, cfg.excludeExts))
                reject = true;
        }

        if (reject)
        {
            ++st.skipped; // 필터로 제외된 것 스킵
            continue;
        }

        out.push_back(e);
    }

    return out;
}


//============================= 콘솔 표 출력 =============================
static void PrintTableHeader()
{
    cout << "=== 결과(미리보기) ===" << endl;
    cout << std::left
        << std::setw(6) << "No"
        << std::setw(12) << "Size"
        << std::setw(8) << "Ext"
        << "Path" << endl;

    cout << std::string(6 + 12 + 8 + 90, '-') << endl;//구분선
}

static void PrintEntriesTable(std::vector<FileEntry> entries, std::size_t topN)
{
    std::sort(entries.begin(), entries.end(),
        [](const FileEntry& a, const FileEntry& b) { return a.size > b.size; });

    PrintTableHeader();

    std::size_t n = std::min(topN, entries.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& e = entries[i];
        std::string pathStr = e.path.u8string();

        cout << std::left
            << std::setw(6) << (i + 1)
            << std::setw(12) << e.size
            << std::setw(8) << e.ext;

        //문자깨짐현상으로 인한 UTF-8 콘솔 출력 지원(윈도우)
#ifdef _WIN32
        std::wstring wpath = Utf8ToWide(pathStr);
        std::wstring wpathTr = TruncateMiddleW(wpath, 100);
        ConsoleWriteWide(wpathTr);
        cout << endl;
#else
        cout << pathStr << endl;
#endif
    }

    if (entries.size() > topN)
        cout << "... (" << (entries.size() - topN) << "개 더 있음)" << endl;
}


//============================= CSV 저장 =============================

// CSV 출력
static std::string EscapeCsv(const std::string& s)

//따옴표검사
{
    bool needQuote = false;
    for (char c : s)
    {
        if (c == '"' || c == ',' || c == '\n' || c == '\r')
        {
            needQuote = true;
            break;
        }
    }
    if (!needQuote) return s;

    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s)
    {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// entries를 CSV로 저장
static bool WriteCsv(const std::string& outPath, const std::vector<FileEntry>& entries)
{
    //폴더가 없으면 생성 시도(실패시 ofstream에서 다시 걸러짐)
    std::error_code ec;
    fs::path p(outPath);
    if (p.has_parent_path())
        fs::create_directories(p.parent_path(), ec);

    //파일에 쓰기
    std::ofstream ofs(outPath, std::ios::binary);
    if (!ofs) {
        Log("[에러]", "CSV 파일 열기 실패: " + outPath);
        return false;
    }

    ofs << "Path,Size,Ext" << endl;
    for (const auto& e : entries)
    {
        std::string pathStr = e.path.u8string();
        ofs << EscapeCsv(pathStr) << ','
            << e.size << ','
            << EscapeCsv(e.ext) << endl;
    }
    return true;
}


//============================= 출력 =============================

static void PrintSummary(const Stats& st, std::size_t filteredCount)
{
    cout << "\n=== 요약 ===" << endl;
    cout << "총 스캔항목: " << st.total << endl;
    cout << " 폴더: " << st.dirs << endl;
    cout << " 파일: " << st.files << endl;
    cout << " 스킵항목: " << st.skipped << endl;
    cout << " 필터 통과 파일 수: " << filteredCount << endl;
    cout << " 데이터수집 성공: " << st.data_ok << endl;
    cout << " 데이터수집 실패: " << st.data_fail << endl;
}


//============================= main =============================

int main(int num, char* values[])
{
    //문자깨짐현상으로 인한 콘솔 UTF-8 설정(윈도우)
#ifdef _WIN32
    SetConsoleOutputCP(949);
    SetConsoleCP(949);
#endif


    //로그 파일 열기
    g_log.open("scanner.log", std::ios::app);
    if (!g_log.is_open())
        cout << "[경고] 로그 파일(scanner.log) 열기 실패이외다." << endl;
    else
        Log("[INFORMATION]", "프로그램 시작일세");

    // 어떤 경로로 return 하든 반드시 종료 로그 + flush/close를 보장
    auto _scope = MakeScopeGuard([&]()
        {
            Log("[INFORMATION]", "프로그램을 종료하겠소이다");
            if (g_log.is_open())
            {
                g_log.flush();
                g_log.close();
            }
        });

    // values 파싱(--out / 스캔경로)
    CliOptions opt = ParseArgs(num, values);

    // 경로 받기
    std::string path = GetPathFromArgsOrInput(num, values, opt);

    // 유효성 검사
    if (!ValidatePath(path))
        return 1;

    Log("[INFORMATION]", "스캔을 시작하겠네: " + path);

    // 스캔 + 데이터 수집
    Stats st;
    auto entries = ScanDirectory(path, st);

    // 크기필터 설정(프로토타입 필터값)
    FilterConfig cfg;
    cfg.minSize = 1;
    // 포함 확장자(프로토타입 필터값)
    cfg.includeExts = { ".exe", ".dll", ".sys", ".txt" };
    // 제외 확장자(프로토타입 필터값)
    cfg.excludeExts = { ".tmp", ".log" };

    auto filtered = FilterEntries(entries, cfg, st);

    // 표 출력(상위 10개)
    PrintEntriesTable(filtered, 10);

    // CSV 저장 경로 결정
    // 저장경로 없이 그냥 엔터치면 기본값 "report.csv"로 저장
    std::string outPath = opt.outPath;

    if (outPath.empty())
    {
        cout << "\nCSV 저장 경로를 입력하시게나: ";
        std::getline(std::cin, outPath);
        if (outPath.empty())
            outPath = "report.csv";
    }

    // CSV 저장
    if (!WriteCsv(outPath, filtered))
    {
        cout << "\n[오류] CSV 저장 실패이외다: " << outPath << endl;
        Log("[에러]", "CSV 저장 실패일세: " + outPath);
    }
    else
    {
        cout << "\n[완료] CSV 저장 완료이외다: " << outPath << endl;
        Log("[INFORMATION]", "CSV 저장 완료일세: " + outPath);
    }

    // 요약 출력
    PrintSummary(st, filtered.size());

    return 0;
}



// 프로그램 실행: <Ctrl+F5> 또는 [디버그] > [디버깅하지 않고 시작] 메뉴
// 프로그램 디버그: <F5> 키 또는 [디버그] > [디버깅 시작] 메뉴

// 시작을 위한 팁: 
//   1. [솔루션 탐색기] 창을 사용하여 파일을 추가/관리합니다.
//   2. [팀 탐색기] 창을 사용하여 소스 제어에 연결합니다.
//   3. [출력] 창을 사용하여 빌드 출력 및 기타 메시지를 확인합니다.
//   4. [오류 목록] 창을 사용하여 오류를 봅니다.
//   5. [프로젝트] > [새 항목 추가]로 이동하여 새 코드 파일을 만들거나, [프로젝트] > [기존 항목 추가]로 이동하여 기존 코드 파일을 프로젝트에 추가합니다.
//   6. 나중에 이 프로젝트를 다시 열려면 [파일] > [열기] > [프로젝트]로 이동하고 .sln 파일을 선택합니다.
