// EndpointFileScanner.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include <iostream>
#include <string>
#include <filesystem>
#include <system_error>
#include <vector>
#include <cctype>

namespace fs = std::filesystem;
using std::cout;
using std::endl;

//파일의 상세정보 기록
struct FileEntry
{
    fs::path path;
    std::uintmax_t size = 0;
    std::string ext;
    fs::file_time_type mtime;
};

//파일의 전체합계/통계 저장
struct Stats
{
    std::size_t total = 0;      //총 스캔항목수
    std::size_t files = 0;      //파일로 판정된 항목수
    std::size_t skipped = 0;    //제외된 항목수(오류/필터 제외 포함)

    std::size_t data_ok = 0;    //데이터수집이 완료된 항목수
    std::size_t data_fail = 0;  //데이터수집에 실패한항목수
};

//문자열을 소문자로 변환후 돌려줌
static std::string ToLower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

//경로가 인자로 들어오면 사용/없으면 사용자입력
static std::string GetPathFromArgsOrInput(int num, char* values[])
{
    std::string path;
    if (num >= 2) {
        path = values[1];
    }
    else {
        cout << "스캔할 폴더 경로를 입력하시게나: ";
        std::getline(std::cin, path);
    }
    return path;
}

// 경로유효성 검사
static bool ValidatePath(const std::string& path)
{
    std::error_code ec;

    if (path.empty()) {
        cout << "[오류] 경로가 비어있는게로구먼." << endl;
        return false;
    }

    ec.clear();
    bool exists = fs::exists(path, ec);
    if (ec || !exists) {
        cout << "[오류] 경로 접근/확인 실패이외다: " << path;
        if (ec) cout << " (" << ec.message() << ")";
        cout << endl;
        return false;
    }

    ec.clear();
    bool isDir = fs::is_directory(path, ec);
    if (ec || !isDir) {
        cout << "[오류] 폴더가 아닌게지라: " << path;
        if (ec) cout << " (" << ec.message() << ")";
        cout << endl;
        return false;
    }

    return true;
}

// -------------------- 스캔 + 데이터 수집 --------------------

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
        cout << "[오류] 디렉터리 순환 시작 실패: " << ec.message() << endl;
        return entries;
    }

    for (; it != end; it.increment(ec))
    {
        ++st.total;

        // 순회중 오류가 있다면 스킵하고 계속
        if (ec) {
            ++st.skipped;
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

            fe.size = entry.file_size(mec);
            if (mec) {
                ++st.data_fail;
                ++st.skipped;
                continue;
            }

            fe.mtime = entry.last_write_time(mec);
            if (mec) {
                ++st.data_fail;
                ++st.skipped;
                continue;
            }

            ++st.data_ok;
            entries.push_back(std::move(fe));
        }
        //파일 판정 자체가 실패한 경우 스킵
        else if (iec)
        {
            // 파일 판정 자체가 실패한 경우 스킵
            ++st.skipped;
        }
        // 정규 파일이 아닌 항목(폴더/링크 등)은 통과
    }

    return entries;
}

// -------------------- Day4: 필터링 --------------------

struct FilterConfig
{
    std::uintmax_t minSize = 0;              // 최소 크기
    std::uintmax_t maxSize = 0;              // 최대 크기

    std::vector<std::string> includeExts;    // 포함 확장자 목록(비어있다면 모두허용)
    std::vector<std::string> excludeExts;    // 제외 확장자 목록(비어있다면 모두허용)
};

//확장자가 확장자 목록에 있는지 검사
static bool InListInsensitive(const std::string& val, const std::vector<std::string>& list)
{
    for (const auto& x : list)
        if (ToLower(val) == ToLower(x))
            return true;
    return false;
}

static std::vector<FileEntry> FilterEntries(const std::vector<FileEntry>& entries,
    const FilterConfig& cfg,
    Stats& st)
{
    std::vector<FileEntry> out;
    out.reserve(entries.size());

    for (const auto& e : entries)
    {
        bool reject = false;

        // 1) 크기 필터
        if (cfg.minSize != 0 && e.size < cfg.minSize) reject = true;
        if (cfg.maxSize != 0 && e.size > cfg.maxSize) reject = true;

        // 2) 포함확장자/제외확장자
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

// -------------------- 출력 --------------------

static void PrintSummary(const Stats& st, std::size_t filteredCount)
{
    cout << "\n=== 요약 ===" << endl;
    cout << "총 스캔항목: " << st.total << endl;
    cout << " 파일: " << st.files << endl;
    cout << " 스킵항목: " << st.skipped << endl;
    cout << " 필터 통과 파일 수: " << filteredCount << endl;
    cout << " 데이터수집 성공: " << st.data_ok << endl;
    cout << " 데이터수집 실패: " << st.data_fail << endl;
}

int main(int num, char* values[])
{
    // 경로 받기
    std::string path = GetPathFromArgsOrInput(num, values);

    // 유효성 검사
    if (!ValidatePath(path))
        return 1;

    // 스캔 + 데이터 수집
    Stats st;
    auto entries = ScanDirectory(path, st);

    // 필터 설정(프로토타입 값)
    FilterConfig cfg;
    cfg.minSize = 1; // 0바이트 파일 제외
    cfg.includeExts = { ".exe", ".dll", ".sys", ".txt" };
    cfg.excludeExts = { ".tmp", ".log" };

    auto filtered = FilterEntries(entries, cfg, st);

    //요약출력
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
