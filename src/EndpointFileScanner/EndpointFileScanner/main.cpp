// EndpointFileScanner.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

//-  v0.2
#include <iostream>
#include <string>
#include <filesystem>
#include <system_error>
//-  v0.4
#include <vector>
#include <cctype> 

//-  v0.2
namespace fs = std::filesystem;
using std::cout;
using std::endl;

//-  v0.4
struct FileEntry
{
    fs::path path;
    std::uintmax_t size = 0;
    std::string ext;
    fs::file_time_type mtime;
};

//-  v0.4
static std::string ToLower(std::string s)
{
    for (char& c : s)c = (char)std::tolower((unsigned char)c);
    return s;
}


//-  v0.2
int main(int num, char* values[])

{
    std::string path;

    //경로받기//
    if (num >= 2) {
        path = values[1];
    }
    else {
        std::cout << "스캔할 폴더 경로를 입력하시게나: ";
        std::getline(std::cin, path);
    }

    std::error_code ec;

   
    //유효성검사
    if (path.empty()) {
        std::cout << "[오류] 경로가 비어있는게로구먼."<<endl;
        return 1;
    }

    //에러코드 초기화후 경로검사
    ec.clear();
    bool exists = fs::exists(path, ec);
    if(ec||!exists)
    {
        cout << "[오류] 경로 접근/확인 실패이외다: " << path;
        if(ec) cout<< " (" << ec.message() << ")";
        cout << endl;
        return 1;

    }
    //에러코드 초기화후 폴더확인
    ec.clear();
    bool isDir = fs::is_directory(path, ec);
    if (ec || !isDir)
    {
        std::cout << "[오류] 폴더가 아닌게지라: " << path;
        if(ec) cout<< " (" << ec.message() << ")";
        cout << endl;
        return 1;
    }

    //재귀순환-  v0.3//
    std::size_t total = 0;
    std::size_t files = 0;
    std::size_t skipped = 0;

    //데이터수집 성공/실패-  v0.4
    std::size_t data_ok = 0;
    std::size_t data_fail = 0;

    //FileEntry 저장-  v0.4
    std::vector<FileEntry> entries;
    entries.reserve(2048);

    ec.clear();
    fs::recursive_directory_iterator start(path, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;


    //-  v0.3
    if (ec)
    {
        cout << "[오류] 디렉터리 순환 시작 실패:" << ec.message() << endl;
        return 1;
    }
    for(; start!=end; start.increment(ec))
    {
        ++total;
        //접근에 문제가있다면 스킵하고 계속
        if (ec)
        {
            ++skipped;
            ec.clear();
            continue;
        }
        const fs::directory_entry& entry = *start;

        //ec와 분리 ::파일판정은 iec로 
        std::error_code iec;
        if (entry.is_regular_file(iec))
        {
            ++files;

            //데이터수집 -  v0.4
            FileEntry fe;
            fe.path = entry.path();
            fe.ext = ToLower(fe.path.extension().string());

            std::error_code mec;

            fe.size = entry.file_size(mec);
            if (mec)
            {
                ++data_fail; ++skipped;
                continue;
            }
            fe.mtime = entry.last_write_time(mec);
            if (mec)
            {
                ++data_fail; ++skipped;
                continue;
            }
            ++data_ok; 
            entries.push_back(std::move(fe));
        }
        else if (iec)
        {
            ++skipped;
        }    
    }
    
    //-  v0.3
    cout << "=== 요약 ===" << endl;
    cout << "총 스캔항목: " << total << endl;
    cout << " 파일: " << files << endl;
    cout << " 스킵항목: " << skipped << endl;
    //-  v0.4
    cout << " 데이터수집 파일 수: " << entries.size() << endl;
    cout << " 데이터수집 성공: " << data_ok << endl;
    cout << " 데이터수집 실패: " << data_fail << endl;
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
