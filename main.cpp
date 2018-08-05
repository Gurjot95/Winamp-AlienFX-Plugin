/*

Winamp generic plugin template code.
This code should be just the basics needed to get a plugin up and running.
You can then expand the code to build your own plugin.

Updated details compiled June 2009 by culix, based on the excellent code examples
and advice of forum members Kaboon, kichik, baafie, burek021, and bananskib.
Thanks for the help everyone!

*/

#include "main.h"
#include "alienfx.h"
#include "timer.h"
#include <deque>
#include <string>
#include <vector>
#include <boost/scoped_ptr.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include "kiss_fft.h"
#include "window.h"
#include <GL/glu.h>
#include "config.h"
#include "Slider.h"
#include "XmlConfig.h"
#include "XmlDevices.h"
#include "Zone.h"
#include "debug.h"
#ifdef USE_STACK_WALKER
#include "MyStackWalker.h"
#endif

#ifdef USE_STACK_WALKER
// Exception handling and stack-walking
LONG WINAPI ExpFilter(EXCEPTION_POINTERS* pExp, DWORD dwExpCode) {
	MyStackWalker sw;
	sw.ShowCallstack(GetCurrentThread(), pExp->ContextRecord);
	return EXCEPTION_EXECUTE_HANDLER;
}
#endif

float round2(float pValue) {
	return floor(pValue + 0.5f);
}

enum InitState {
	INIT_IN_PROGRESS,
	INIT_SUCCESSFULL,
	INIT_FAILED
};

HWND hOtherWindow = NULL;
boost::thread *worker = NULL;
float energy_factor = 1.3f;
float variance_factor = 150.0f;
extern bool AlienfxNew;
extern Slider *EnergySlider[8];
extern Slider *VarianceSlider[8];
extern Slider *FactorSlider[2];
bool test = false;
volatile InitState threadInitState = INIT_IN_PROGRESS;
extern bool RenderWindowVisible;

winampVisHeader header = {
  0x101,
  "Alienfx Visualization plugin",
  GetModule
};

bool ReplaceEnvironmentVars(std::string& str);

winampVisModule module;
void FillModule() {
	module.description = "AlienFX Visualization plugin";
	module.hwndParent = NULL;
	module.hDllInstance = NULL;
	module.sRate = 0;
	module.nCh = 0;
	module.latencyMs = 0;
	module.delayMs = 0;

	module.spectrumNCh = 2;
	module.waveformNCh = 2;

	memset(module.spectrumData, 0, 2 * 576 * sizeof(byte));
	memset(module.waveformData, 0, 2 * 576 * sizeof(byte));

	module.config = config;
	module.init = init;
	module.render = render;
	module.quit = quit;
	module.userData = NULL;

	LoadXmlConfigPart1(module.delayMs);
}

sys::Zeitpunkt *LastRender = NULL;
//sys::Zeitpunkt *LastActionZ = NULL;
std::vector< std::deque<float> > avgs;
enum BeatState {
	BS_ON,
	BS_OFF,
	BS_WAIT
};
BeatState beat[32];
bool running = true;

// event functions follow

byte LastData[2][576];
kiss_fft_cpx FftIn[2][1024];
kiss_fft_cpx FftOut[1024];
kiss_fft_cfg  kiss_fft_state;
int CurrentFft = 0;
int CurrentPos = 0;

float EnergyValues[32];
float VarianceValues[32];

#ifdef USE_STACK_WALKER
int MyRender(winampVisModule* pVisModule);
#endif

int render(winampVisModule* pVisModule) {
	if (!running)
		return -1;
#	ifdef USE_STACK_WALKER
	__try {
		return MyRender(pVisModule);
	}
	__except (ExpFilter(GetExceptionInformation(), GetExceptionCode())) {
		MessageBoxA(NULL, "Fatal error in render. see alienfx_vis_crashlog for details", "Error", MB_OK | MB_ICONERROR);
		return -1;
	}
}
int MyRender(winampVisModule* pVisModule) {
#endif

	bool NewFft = false;
	sys::Zeitpunkt now(sys::MainTimer);
	int i = 0;
	for (i = 0; i < 2 * 576; i++) {
		if (module.spectrumData[i] != LastData[i])
			break;
	}
	if (i < 2 * 576) // Do we have new data?
	{
		//Do FFT
		for (i = 0; CurrentPos < 1024 && i < 576; CurrentPos++, i++) {
			FftIn[CurrentFft][CurrentPos].r = (static_cast<float>(module.waveformData[0][i])
				+ static_cast<float>(module.waveformData[1][i]))
				/ 254.0f;
			FftIn[CurrentFft][CurrentPos].i = 0;
		}
		if (i != 576) { //Current Fft Data is full
			NewFft = true;
			int NextFft = (CurrentFft == 0) ? 1 : 0;
			CurrentPos = 0;
			for (; i < 576; i++, CurrentPos++) {
				FftIn[NextFft][CurrentPos].r = (static_cast<float>(module.waveformData[0][i])
					+ static_cast<float>(module.waveformData[1][i]))
					/ 254.0f;
				FftIn[NextFft][CurrentPos].i = 0;
			}
		}
	}
	if (NewFft) { //Do we have new fft data
	  //Do the actual FFT
		kiss_fft(kiss_fft_state, &FftIn[CurrentFft][0], FftOut);
		CurrentFft = (CurrentFft == 0) ? 1 : 0;

		//Last render was to long ago. Propably paused or new song.
		//Reset the average data
		if (now - *LastRender > 100) {
			for (int i = 0; i < 64; i++)
				avgs[i].resize(0);
		}

		//Do the actual beat detection
		int start = 0;
		int ignore = 0; //ignore the first 3 subbands
		for (int i = 0; i < 32; i++) {
			int num = i / 4;
			//int size = round((double)(i+1) * (384.0/2016.0) + (3687.0/2016.0)); // 64 subbands
			int size = round((double) (i + 1) * (448.0 / 496.0) + (544.0 / 496.0));
			if (start + size >= 512)
				size = 512 - start;
			float local_energy = 0.0f;
			for (int j = start; j < start + size; j++) {
				local_energy += sqrt(FftOut[j].r * FftOut[j].r + FftOut[j].i * FftOut[j].i);
			}
			local_energy = local_energy / size;

			float average_energy = 0.0f;
			for (std::deque<float>::iterator it = avgs[i].begin(); it != avgs[i].end(); ++it) {
				average_energy += *it;
			}
			average_energy /= (float) avgs[i].size();

			float variance = 0.0f;
			for (std::deque<float>::iterator it = avgs[i].begin(); it != avgs[i].end(); ++it) {
				float temp1 = ((*it / average_energy) - 1.0f) * 12.0f;
				float temp2 = *it - average_energy;
				variance += ((temp2 * temp2 * FactorSlider[0]->GetValue()) + (temp1 * temp1 * FactorSlider[1]->GetValue())) / (FactorSlider[0]->GetValue() + FactorSlider[1]->GetValue());
			}
			variance /= avgs[i].size();

			EnergyValues[i] = local_energy / average_energy;
			VarianceValues[i] = variance;

			//Beatdetection nur wenn wir auch eine kompeltte History haben
			if (avgs[i].size() >= 43) {
				ConfigDraw(i, local_energy / average_energy, variance);

				if (local_energy > average_energy * EnergySlider[num]->GetValue() && variance > VarianceSlider[num]->GetValue() && i >= ignore) {
					beat[i] = BS_ON;
				}
			}
			avgs[i].push_back(local_energy);
			while (avgs[i].size() > 43)
				avgs[i].pop_front();
			start += size;
		}
		memcpy(LastData, module.spectrumData, sizeof(byte) * 2 * 576);
	}

	if (RenderWindowVisible) {

		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glLoadIdentity();

		/*glBegin(GL_LINE_STRIP);
		glColor3f(1.0,0.0,0.0);
		for(int i=0;i<576;i++){
			float x = (float)i / 575.0f;
			float y = (float)module.waveformData[0][i] / 127.0f;
			glVertex2f(x,y);
		}
		glEnd();

		glBegin(GL_LINE_STRIP);
		glColor3f(0.0,1.0,0.0);
		for(int i=0;i<576;i++){
			float x = (float)i / 575.0f;
			float y = (float)module.waveformData[1][i] / 127.0f;
			glVertex2f(x,y);
		}
		glEnd();

		glBegin(GL_LINE_STRIP);
		glColor3f(1.0,0.0,0.0);
		for(int i=0;i<576;i++){
			float x = (float)i / 575.0f - 1.0f;
			float y = (float)module.spectrumData[0][i] / 255.0f * 2.0 - 1.0f;
			glVertex2f(x,y);
		}

		glEnd();

		glBegin(GL_LINE_STRIP);
		glColor3f(0.0,1.0,0.0);
		for(int i=0;i<576;i++){
			float x = (float)i / 575.0f - 1.0f;
			float y = (float)module.spectrumData[1][i] / 255.0f * 2.0 - 1.0f;
			glVertex2f(x,y);
		}
		glEnd();

		glBegin(GL_LINE_STRIP);
		glColor3f(1.0,1.0,0.0f);
		for(int i=0;i<512;i++){
			float x = (float)i / 511.0f - 1.0f;
			float y = sqrt(FftOut[i].r * FftOut[i].r + FftOut[i].i * FftOut[i].i) * 1.0/100.0 - 1.0f;
			glVertex2f(x,y);
		}
		glEnd();*/

		for (int i = 0; i < 32; i++) {
			int num = i / 4;
			glBegin(GL_LINE_STRIP);
			if (EnergyValues[i] > EnergySlider[num]->GetValue())
				glColor3f(1.0f, 0.0f, 0.0f);
			else
				glColor3f(0.0f, 1.0f, 0.0f);
			float y = EnergyValues[i] / 2.0f - 1.0f;
			float x = 1.0f / 32.0f * i - 1.0f + 1.0f / 128.0f;
			glVertex2f(x, -1.0f);
			glVertex2f(x, y);
			x += 1.0f / 32.0f - 1.0f / 128.0f;
			glVertex2f(x, y);
			glVertex2f(x, -1.0f);
			glEnd();
		}

		glBegin(GL_LINE_STRIP);
		glColor3f(1.0f, 1.0f, 0.0f);
		for (int i = 0; i < 8; i++) {
			float x = 1.0f / 8.0f * i - 1.0f;
			float y = EnergySlider[i]->GetValue() / 2.0f - 1.0f;
			glVertex2f(x, y);
			glVertex2f(x + 1.0f / 8.0f, y);
		}
		glEnd();

		for (int i = 0; i < 32; i++) {
			int num = i / 4;
			glBegin(GL_LINE_STRIP);
			if (VarianceValues[i] > VarianceSlider[num]->GetValue())
				glColor3f(1.0f, 0.0f, 0.0f);
			else
				glColor3f(0.0f, 1.0f, 0.0f);
			float x = 1.0f / 32.0f * i + 1.0f / 128.0f;
			float y = VarianceValues[i] / 400.0f * 2.0f - 1.0f;
			glVertex2f(x, -1.0f);
			glVertex2f(x, y);
			x += 1.0f / 32.0f - 1.0f / 128.0f;
			glVertex2f(x, y);
			glVertex2f(x, -1.0f);
			glEnd();
		}

		glBegin(GL_LINE_STRIP);
		glColor3f(1.0f, 1.0f, 0.0f);
		for (int i = 0; i < 8; i++) {
			float x = 1.0f / 8.0f * i;
			float y = VarianceSlider[i]->GetValue() / 400.0f * 2.0f - 1.0f;
			glVertex2f(x, y);
			glVertex2f(x + 1.0f / 8.0f, y);
		}
		glEnd();

		OpenGLSwapBuffers();
	}

	*LastRender = now;
	return 0;
}

int ToAlienfxColor(int pColor) {
	int red = static_cast<float>((pColor & 0xFF0000) >> 16) / 255.0f * 15.0f;
	int green = static_cast<float>((pColor & 0x00FF00) >> 8) / 255.0f * 15.0f;
	int blue = static_cast<float>(pColor & 0x0000FF) / 255.0f * 15.0f;
	return (red << 8) | (green << 4) | blue;
}

#ifdef USE_STACK_WALKER
void MyWork();
#endif

struct WorkCleanup {
	~WorkCleanup() {
		AlienfxDeinit();
	}
};

void work() {
	//#	ifdef USE_STACK_WALKER
	__try {
		MyWork();
	}
	__except (ExpFilter(GetExceptionInformation(), GetExceptionCode())) {
		MessageBoxA(NULL, "Fatal error in work. see alienfx_vis_crashlog for details", "Error", MB_OK | MB_ICONERROR);
	}
}
void MyWork() {
	//#endif
	byte status;

	//Setup
	if (!LoadXmlDevices()) {
		if (!AlienfxInit()) {
			MessageBox(HWND_DESKTOP, L"Alienware Device not found. Plugin not active!", L"Error", MB_OK);

			threadInitState = INIT_FAILED;
			return;
		}
	}

	WorkCleanup cleanup;

	/// test leds
	dprintf("waiting for busy");
	status = AlienfxWaitForBusy();
	dprintf("initial reset (busy status %x)", status);
	AlienfxReset(ALIENFX_ALL_ON);
	Sleep(3);
	dprintf("initial wait for ready");
	status = AlienfxWaitForReady();
	dprintf("initial status %x", status);

	dprintf("running test command");
	AlienfxSetColor(ALIENFX_STAY, 1, 0xFFFFFFFF, 0);
	AlienfxEndLoopBlock();
	AlienfxEndTransmitionAndExecute();

	dprintf("thread init successfull");

	threadInitState = INIT_SUCCESSFULL;

	dprintf("starting alienfx loop");
	while (running) {

		std::vector<ZoneInfo> CommandsToSend;
		Zone **FreqBandUsage = Zone::GetFreqBandUsage();
		for (int i = 0; i < 32; i++) {
			if (FreqBandUsage[i] != NULL) {
				FreqBandUsage[i]->SetUsed(false);
			}
		}

		for (int i = 0; i < 32; i++) {
			if (!running)
				return;
			if (beat[i] != BS_WAIT && FreqBandUsage[i] != NULL) {
				Zone *zone = FreqBandUsage[i];
				if (zone->GetUsed() == false) {
					//dprintf("Found beat");
					zone->SetUsed(true);
					ZoneInfo info;
					info._Leds = zone->GetLedBits();
					info._Code = zone->GetLedCode();
					if (beat[i] == BS_ON) {
					//info._Color = ToAlienfxColor(zone->GetColor()) << 12;
					info._Color = zone->GetColor();
					dprintf("Found Color: %d:%d", zone->GetColor(), info._Color);
				}
					else
						info._Color = 0x000;
					CommandsToSend.push_back(info);
				}
			}
		}

		if (CommandsToSend.size() > 0) {
			//Sending command to alienfx device
			  //dprintf(running ? "true" : "false");
			if (!running) return;
			status = AlienfxWaitForBusy();
			if (status == 0x06) {
				dprintf("Reinit wait for busy");
				Sleep(1000);
				if (!running) return;
				AlienfxReinit();
				continue;
			} else if (status != 0x11) {
				dprintf("Skip 0x11 current 0x%x", status);
				Sleep(50);
				if (!running) return;
				continue;
			}
			AlienfxReset(ALIENFX_ALL_ON);
			Sleep(3);
			if (!running) return;
			status = AlienfxWaitForReady();
			if (status == 0x06) {
				dprintf("Reinit wait for ready");
				Sleep(1000);
				if (!running) return;
				AlienfxReinit();
				continue;
			} else if (status != 0x10) {
				if (status == 0x11) {
					AlienfxReset(ALIENFX_ALL_ON);
					Sleep(3);
					if (!running) return;
					status = AlienfxWaitForReady();
					if (status == 0x06) {
						dprintf("Reinit wait for ready");
						Sleep(1000);
						if (!running) return;
						AlienfxReinit();
						continue;
					}
				} else {
					dprintf("Skip 0x10 current 0x%x", status);
					Sleep(50);
					if (!running) return;
					continue;
				}
			}
			dprintf("Total LED %d", CommandsToSend.size());
			for (unsigned int lightIndex = 0; lightIndex < CommandsToSend.size(); lightIndex++) {
				ZoneInfo& info = CommandsToSend[lightIndex];
				AlienfxSetColor(ALIENFX_STAY, lightIndex, info._Leds, info._Color);
				AlienfxEndLoopBlock();
			}
			// for(size_t i=0;i<CommandsToSend.size();i++){
			 //  ZoneInfo& info = CommandsToSend[i];

		   //			dprintf("sending command for leds %x color %x",info._Leds, info._Color);
			//   AlienfxSetColor(ALIENFX_STAY, i+1, info._Leds, info._Color);
			//   AlienfxEndLoopBlock();
			// }
			AlienfxEndTransmitionAndExecute();

			for (int i = 0; i < 32; i++) {
				switch (beat[i]) {
				case BS_ON:
					beat[i] = BS_OFF;
					break;
				case BS_OFF:
					beat[i] = BS_WAIT;
					break;
				default:
					break;
				}
			}

			Sleep(40);
			if (!running) return;
		} else {
			Sleep(10);
			if (!running) return;
		}

		/*if(beat || test){
		  //SendMyMessage(0,std::wstring(L"beat"),0);
		  beat = false;
		  int color = beat_color;

		  //Sending leds on
		  status = AlienfxWaitForBusy();
		  if(status == 0x06){
			SendMyMessage(0,std::wstring(L"Reinit wait for busy"),0);
			Sleep(1000);
			AlienfxReinit();
			continue;
		  }
		  else if(status != 0x11){
			wchar_t temp[1024];
			swprintf(temp,L"Skip 0x11 current 0x%x",status);
			SendMyMessage(0,std::wstring(temp),0);
			Sleep(50);
			continue;
		  }
		  AlienfxReset(ALIENFX_ALL_ON);
		  Sleep(3);
		  status = AlienfxWaitForReady();
		  if(status == 0x06){
			SendMyMessage(0,std::wstring(L"Reinit wait for ready"),0);
			Sleep(1000);
			AlienfxReinit();
			continue;
		  }
		  else if(status != 0x10){
			if(status == 0x11){
			  AlienfxReset(ALIENFX_ALL_ON);
			  Sleep(3);
			  status = AlienfxWaitForReady();
			  if(status == 0x06){
				SendMyMessage(0,std::wstring(L"Reinit wait for ready"),0);
				Sleep(1000);
				AlienfxReinit();
				continue;
			  }
			}
			else {
			  wchar_t temp[1024];
			  swprintf(temp,L"Skip 0x10 current 0x%x",status);
			  SendMyMessage(0,std::wstring(temp),0);
			  Sleep(50);
			  continue;
			}
		  }
		  AlienfxSetColor(ALIENFX_BATTERY_STATE, 1, alienhead, 0);
		  AlienfxEndLoopBlock();
		  AlienfxSetColor(ALIENFX_STAY, 2, blink_leds, color);
		  AlienfxEndLoopBlock();
		  AlienfxEndTransmitionAndExecute();
		  Sleep(40);

		  //Leds Off
		  status = AlienfxWaitForBusy();
		  if(status == 0x06){
			SendMyMessage(0,std::wstring(L"Reinit wait for busy"),0);
			Sleep(1000);
			AlienfxReinit();
			continue;
		  }
		  else if(status != 0x11){
			SendMyMessage(0,std::wstring(L"Skip 0x11"),0);
			Sleep(50);
			continue;
		  }
		  AlienfxReset(ALIENFX_ALL_ON);
		  Sleep(3);
		  status = AlienfxWaitForReady();
		  if(status == 0x06){
			SendMyMessage(0,std::wstring(L"Reinit wait for ready"),0);
			Sleep(1000);
			AlienfxReinit();
			continue;
		  }
		  else if(status != 0x10){
			if(status == 0x11){
			  AlienfxReset(ALIENFX_ALL_ON);
			  Sleep(3);
			  status = AlienfxWaitForReady();
			  if(status == 0x06){
				SendMyMessage(0,std::wstring(L"Reinit wait for ready"),0);
				Sleep(1000);
				AlienfxReinit();
				continue;
			  }
			}
			else {
			  wchar_t temp[1024];
			  swprintf(temp,L"Skip 0x10 current 0x%x",status);
			  SendMyMessage(0,std::wstring(temp),0);
			  Sleep(50);
			  continue;
			}
		  }
		  AlienfxSetColor(ALIENFX_BATTERY_STATE, 1, alienhead, 0);
		  AlienfxEndLoopBlock();
		  AlienfxSetColor(ALIENFX_STAY, 2, blink_leds, 0);
		  AlienfxEndLoopBlock();
		  AlienfxEndTransmitionAndExecute();
		  Sleep(40);
		  test = false;
		}
		else
		  Sleep(10);*/
	}
}

#ifdef USE_STACK_WALKER
int MyInit(winampVisModule* pVisModule);
#endif

int init(winampVisModule* pVisModule) {
	//#	ifdef USE_STACK_WALKER
	__try {
		return MyInit(pVisModule);
	}
	__except (ExpFilter(GetExceptionInformation(), GetExceptionCode())) {
		MessageBoxA(NULL, "Fatal error in init. see alienfx_vis_crashlog for details", "Error", MB_OK | MB_ICONERROR);
		return 1;
	}
}
int MyInit(winampVisModule* pVisModule) {

	running = true;

	worker = new boost::thread(work);
	for (int i = 0; i < 20 && threadInitState == INIT_IN_PROGRESS; i++) {
		Sleep(1000);
	}
	if (threadInitState != INIT_SUCCESSFULL) {
		MessageBoxA(HWND_DESKTOP, "No AlienFX device found. Plugin will not work", "Error", MB_OK);
		if (!worker->timed_join(boost::posix_time::milliseconds(200))) {
			dprintf("Killing thread");
			TerminateThread(worker->native_handle(), 0);
		} else {
			dprintf("thread exited normally");
		}
		delete worker;
		worker = NULL;
		return 1;
	}

	avgs.resize(64);
	sys::MainTimer = new sys::Timer();
	LastRender = new sys::Zeitpunkt(sys::MainTimer);
	//LastActionZ = new sys::Zeitpunkt(sys::MainTimer);

	//Init fft lib
	kiss_fft_state = kiss_fft_alloc(1024, 0, 0, 0);
	for (int i = 0; i < 32; i++)
		beat[i] = BS_WAIT;

	if (!CreateOpenGLWindow(module.hwndParent, module.hDllInstance)) {
		MessageBox(module.hwndParent, L"Creating OpenGL Window failed", L"Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	if (!CreateConfigWindow(module.hwndParent, module.hDllInstance)) {
		MessageBox(module.hwndParent, L"Creating Config Window failed", L"Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	if (!LoadXmlConfigPart2()) {
		MessageBox(module.hwndParent, L"Reading Xml Config failed", L"Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	//Test if the alienfx device does respond
	//beat_color = 0x0F0000;
	/*test = true;
	Sleep(2500);
	if(test){
	  MessageBox(module.hwndParent,L"The alienfx device does not respond. Please restart winamp. If that does not help, please restart your computer.",L"Error",MB_OK);
	  return 1;
	}*/

	char path[500];
	GetModuleFileNameA(module.hDllInstance, path, 500);

	dprintf("Sample Rate is %i\nInit Complete", module.sRate);
	return 0;
}

#ifdef USE_STACK_WALKER
void MyConfig(winampVisModule* pVisModule);
#endif

void config(winampVisModule* pVisModule) {
	if (!running) return;
#	ifdef USE_STACK_WALKER
	__try {
		MyConfig(pVisModule);
	}
	__except (ExpFilter(GetExceptionInformation(), GetExceptionCode())) {
		MessageBoxA(NULL, "Fatal error in config. see alienfx_vis_crashlog for details", "Error", MB_OK | MB_ICONERROR);
	}
}
void MyConfig(winampVisModule* pVisModule) {
#endif

	ShowConfigWindow();
}

#ifdef USE_STACK_WALKER
void MyQuit(winampVisModule* pVisModule);
#endif

void quit(winampVisModule* pVisModule) {
#	ifdef USE_STACK_WALKER
	__try {
		MyQuit(pVisModule);
	}
	__except (ExpFilter(GetExceptionInformation(), GetExceptionCode())) {
		MessageBoxA(NULL, "Fatal error in quit. see alienfx_vis_crashlog for details", "Error", MB_OK | MB_ICONERROR);
	}
}
void MyQuit(winampVisModule* pVisModule) {
#endif

	XmlDeinit();
	running = false;
	if (!worker->timed_join(boost::posix_time::milliseconds(200))) {
		dprintf("Killing thread");
		TerminateThread(worker->native_handle(), 0);
	} else {
		dprintf("thread exited normally");
	}
	free(kiss_fft_state);
	DestroyOpenGLWindow();
	DestroyConfigWindow();
	delete worker;
	delete LastRender;
	delete sys::MainTimer;
	dprintf("Alienfx Visualization Ended successfully");
	CloseDebugConsole();
}

winampVisModule* GetModule(int which) {
	switch (which) {
	case 0:
		OpenDebugConsole();
		FillModule();
		return &module;
		break;
	default:
		return NULL;
		break;
	}
}

void InitOpenGL() {
	glShadeModel(GL_SMOOTH);							// Enable Smooth Shading
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);				// Black Background
	glClearDepth(1.0f);									// Depth Buffer Setup
	glClearStencil(0);
	glEnable(GL_DEPTH_TEST);							// Enables Depth Testing
	glDepthFunc(GL_LESS);								// The Type Of Depth Testing To Do
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);	// Really Nice Perspective Calculations
	glDisable(GL_BLEND);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glPointSize(10.0f);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluOrtho2D(-1.0, 1.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

// This is an export function called by winamp which returns this plugin info.
// We wrap the code in 'extern "C"' to ensure the export isn't mangled if used in a CPP file.
#ifdef __cplusplus
extern "C"
{
#endif
	DLL_EXPORT winampVisHeader* winampVisGetHeader() {
		return &header;
	}
#ifdef __cplusplus
}
#endif
