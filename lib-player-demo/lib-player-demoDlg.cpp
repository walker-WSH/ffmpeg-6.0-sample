
// lib-player-demoDlg.cpp: 实现文件
//

#include "pch.h"
#include "framework.h"
#include "lib-player-demo.h"
#include "lib-player-demoDlg.h"
#include "afxdialogex.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

ffplay_file_info g_fileinfo;
double play_pts = 0.0;
bool paused = false;
const auto slider_steps = 1000;

void my_ffplayer_event::on_player_log(void* player, int level, const char* text)
{
	switch (level)
	{
	case AV_LOG_PANIC:
	case AV_LOG_FATAL:
	case AV_LOG_ERROR:
	case AV_LOG_WARNING:
	case AV_LOG_INFO:
		_cprintf("%s", text);
		break;

	default:
		break;
	}
}

void my_ffplayer_event::on_stream_ready(const ffplay_file_info& info)
{
	g_fileinfo = info;
	printf("%s audio:%d video:%d duration:%lf hw:%d %dx%d \n", __FUNCTION__,
		info.include_audio, info.include_video, info.duration_seconds, info.hw_decode_used,
		info.width, info.height);
}

void my_ffplayer_event::on_video_frame(std::shared_ptr<AVFrame> frame, double pts_seconds) {
	play_pts = pts_seconds;
}

void my_ffplayer_event::on_audio_frame(std::shared_ptr<uint8_t> data, const AudioParams& params, int samples_per_chn, double pts_seconds) {
	play_pts = pts_seconds;
}

void my_ffplayer_event::on_player_paused() {
	paused = true;
}

void my_ffplayer_event::on_player_resumed() {
	paused = false;
}

// 用于应用程序“关于”菜单项的 CAboutDlg 对话框
class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();

// 对话框数据
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV 支持

// 实现
protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()


// ClibplayerdemoDlg 对话框

ClibplayerdemoDlg::ClibplayerdemoDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_LIBPLAYERDEMO_DIALOG, pParent)
{
	AllocConsole();
	global_init();
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

ClibplayerdemoDlg::~ClibplayerdemoDlg() 
{
	global_uninit();
	FreeConsole();
}

void ClibplayerdemoDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_SLIDER_PROGRESS, m_slider);
}

BEGIN_MESSAGE_MAP(ClibplayerdemoDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_BUTTON_PAUSE_RESUME, &ClibplayerdemoDlg::OnBnClickedButtonPauseResume)
	ON_BN_CLICKED(IDC_BUTTON_NEXT, &ClibplayerdemoDlg::OnBnClickedButtonNext)
	ON_WM_DESTROY()
	ON_WM_TIMER()
	ON_NOTIFY(NM_THEMECHANGED, IDC_SLIDER_PROGRESS, &ClibplayerdemoDlg::OnNMThemeChangedSliderProgress)
	ON_NOTIFY(NM_RELEASEDCAPTURE, IDC_SLIDER_PROGRESS, &ClibplayerdemoDlg::OnNMReleasedcaptureSliderProgress)
	ON_NOTIFY(TRBN_THUMBPOSCHANGING, IDC_SLIDER_PROGRESS, &ClibplayerdemoDlg::OnTRBNThumbPosChangingSliderProgress)
END_MESSAGE_MAP()


// ClibplayerdemoDlg 消息处理程序

BOOL ClibplayerdemoDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// 将“关于...”菜单项添加到系统菜单中。

	// IDM_ABOUTBOX 必须在系统命令范围内。
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != nullptr)
	{
		BOOL bNameValid;
		CString strAboutMenu;
		bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
		ASSERT(bNameValid);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	// 设置此对话框的图标。  当应用程序主窗口不是对话框时，框架将自动
	//  执行此操作
	SetIcon(m_hIcon, TRUE);			// 设置大图标
	SetIcon(m_hIcon, FALSE);		// 设置小图标

	// TODO: 在此添加额外的初始化代码
	m_slider.SetRange(0, slider_steps);
	m_slider.SetPos(0);

	{
		ffplay_parameters parameters;
		parameters.file_name = "test.mp3";
		parameters.loop = 1;
		parameters.hw_decode = true;
		parameters.disable_debug_render = true;

		cb = std::make_shared<my_ffplayer_event>();

		player = create_ffplayer(cb);
		if (FFPLAY_ERROR_OK == player->run_player(parameters))
		{
			SetTimer(2000, 100, nullptr);
		}
	}

	return TRUE;  // 除非将焦点设置到控件，否则返回 TRUE
}

void ClibplayerdemoDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialogEx::OnSysCommand(nID, lParam);
	}
}

// 如果向对话框添加最小化按钮，则需要下面的代码
//  来绘制该图标。  对于使用文档/视图模型的 MFC 应用程序，
//  这将由框架自动完成。

void ClibplayerdemoDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // 用于绘制的设备上下文

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// 使图标在工作区矩形中居中
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// 绘制图标
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

//当用户拖动最小化窗口时系统调用此函数取得光标
//显示。
HCURSOR ClibplayerdemoDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void ClibplayerdemoDlg::OnBnClickedButtonPauseResume()
{
	if (player->is_stream_ready())
		player->request_toggle_pause_resume();
}


void ClibplayerdemoDlg::OnBnClickedButtonNext()
{
	if (player->is_stream_ready())
		player->request_step_to_next_frame();
}


void ClibplayerdemoDlg::OnDestroy()
{
	CDialogEx::OnDestroy();

	// TODO: 在此处添加消息处理程序代码
	cb.reset();
	if (player) {
		player->stop_player();
		player.reset();
	}
}


void ClibplayerdemoDlg::OnTimer(UINT_PTR nIDEvent)
{
	// TODO: 在此添加消息处理程序代码和/或调用默认值
	if (!paused && g_fileinfo.duration_seconds > 0) {
		auto percent = play_pts / g_fileinfo.duration_seconds;
		auto pos = int(percent * slider_steps);
		if (pos != m_slider.GetPos())
			m_slider.SetPos(pos);
	}


	CDialogEx::OnTimer(nIDEvent);
}


void ClibplayerdemoDlg::OnNMThemeChangedSliderProgress(NMHDR* pNMHDR, LRESULT* pResult)
{
	// 该功能要求使用 Windows XP 或更高版本。
	// 符号 _WIN32_WINNT 必须 >= 0x0501。
	// TODO: 在此添加控件通知处理程序代码
	*pResult = 0;

}


void ClibplayerdemoDlg::OnNMReleasedcaptureSliderProgress(NMHDR* pNMHDR, LRESULT* pResult)
{
	// TODO: 在此添加控件通知处理程序代码
	*pResult = 0;


	auto percent = (double)m_slider.GetPos() / (double)slider_steps;
	if (player->is_stream_ready())
		player->request_seek_file(percent);
}


void ClibplayerdemoDlg::OnTRBNThumbPosChangingSliderProgress(NMHDR* pNMHDR, LRESULT* pResult)
{
	// 此功能要求 Windows Vista 或更高版本。
	// _WIN32_WINNT 符号必须 >= 0x0600。
	NMTRBTHUMBPOSCHANGING* pNMTPC = reinterpret_cast<NMTRBTHUMBPOSCHANGING*>(pNMHDR);
	// TODO: 在此添加控件通知处理程序代码
	*pResult = 0;
}
