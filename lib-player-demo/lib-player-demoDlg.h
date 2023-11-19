
// lib-player-demoDlg.h: 头文件
//

#pragma once
#include "ffplay.h"
#include <conio.h>

#define ADD_LOG override {_cprintf("%s \n", __FUNCTION__);}
class my_ffplayer_event : public ffplayer_event
{
public:
	virtual ~my_ffplayer_event() = default;

	virtual void on_video_frame(std::shared_ptr<AVFrame> frame, double pts_seconds);
	virtual void on_audio_frame(std::shared_ptr<uint8_t> data, const AudioParams& params, int samples_per_chn, double pts_seconds);

	void on_player_log(void* player, int level, const char* text) override;
	void on_stream_ready(const ffplay_file_info& info) override;
	void on_stream_error(const std::string& error) ADD_LOG
		void on_player_paused();
		void on_player_resumed();
	void on_stream_eof()ADD_LOG
	void on_player_restart()ADD_LOG
	void on_player_auto_exit()ADD_LOG
};

class ClibplayerdemoDlg : public CDialogEx
{
	std::shared_ptr<my_ffplayer_event> cb;
	std::shared_ptr<ffplayer_interface> player;
	CSliderCtrl m_slider;
	CSliderCtrl m_sliderSeek;

public:
	ClibplayerdemoDlg(CWnd* pParent = nullptr);	// 标准构造函数
	virtual ~ClibplayerdemoDlg();

// 对话框数据
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_LIBPLAYERDEMO_DIALOG };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV 支持


// 实现
protected:
	HICON m_hIcon;

	// 生成的消息映射函数
	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	DECLARE_MESSAGE_MAP()
public:
	afx_msg void OnBnClickedButtonPauseResume();
	afx_msg void OnBnClickedButtonNext();
	afx_msg void OnDestroy();
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnNMReleasedcaptureSliderSeek(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnBnClickedButtonSeek();
};
