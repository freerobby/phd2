/*
*  cam_ZWO.cpp
*  PHD Guiding
*
*  Created by Robin Glover.
*  Copyright (c) 2014 Robin Glover.
*  All rights reserved.
*
*  This source code is distributed under the following "BSD" license
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions are met:
*    Redistributions of source code must retain the above copyright notice,
*     this list of conditions and the following disclaimer.
*    Redistributions in binary form must reproduce the above copyright notice,
*     this list of conditions and the following disclaimer in the
*     documentation and/or other materials provided with the distribution.
*    Neither the name of Craig Stark, Stark Labs nor the names of its
*     contributors may be used to endorse or promote products derived from
*     this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
*  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
*  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
*  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
*  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
*  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
*  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
*  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
*  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
*  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
*/
#include "phd.h"

#ifdef ZWO_ASI

#include "cam_ZWO.h"
#include "cameras/ASICamera2.h"

#ifdef __WINDOWS__

#ifdef OS_WINDOWS
// troubleshooting with the libusb definitions
#  undef OS_WINDOWS
#endif

# include <Shlwapi.h>
# include <DelayImp.h>
#endif

Camera_ZWO::Camera_ZWO()
    : m_buffer(0),
    m_capturing(false)
{
    Name = _T("ZWO ASI Camera");
    Connected = false;
    m_hasGuideOutput = true;
    HasSubframes = true;
    HasGainControl = true; // workaround: ok to set to false later, but brain dialog will frash if we start false then change to true later when the camera is connected
}

Camera_ZWO::~Camera_ZWO()
{
    delete[] m_buffer;
}

wxByte Camera_ZWO::BitsPerPixel()
{
    return 8;
}

inline static int cam_gain(int minval, int maxval, int pct)
{
    return minval + pct * (maxval - minval) / 100;
}

inline static int gain_pct(int minval, int maxval, int val)
{
    return (val - minval) * 100 / (maxval - minval);
}

#ifdef __WINDOWS__

#define FACILITY_VISUALCPP  ((LONG)0x6d)
#define VcppException(sev,err)  ((sev) | (FACILITY_VISUALCPP<<16) | err)

static LONG WINAPI DelayLoadDllExceptionFilter(PEXCEPTION_POINTERS pExcPointers, wxString *err)
{
    LONG lDisposition = EXCEPTION_EXECUTE_HANDLER;
    PDelayLoadInfo pdli = PDelayLoadInfo(pExcPointers->ExceptionRecord->ExceptionInformation[0]);

    switch (pExcPointers->ExceptionRecord->ExceptionCode)
    {
    case VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND): {
        // ASICamera2.dll depends on the VC++ 2008 runtime, check for that
        HMODULE hm = LoadLibraryEx(_T("MSVCR90.DLL"), NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (hm)
        {
            FreeLibrary(hm);
            *err = wxString::Format(_("Could not load DLL %s"), pdli->szDll);
        }
        else
            *err = _("The ASI camera library requires the Microsoft Visual C++ 2008 Redistributable Package (x86), available at http://www.microsoft.com/en-us/download/details.aspx?id=29");
        break;
    }

    case VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND):
        if (pdli->dlp.fImportByName)
            *err = wxString::Format("Function %s was not found in %s", pdli->dlp.szProcName, pdli->szDll);
        else
            *err = wxString::Format("Function ordinal %d was not found in %s", pdli->dlp.dwOrdinal, pdli->szDll);
        break;

    default:
        // Exception is not related to delay loading
        lDisposition = EXCEPTION_CONTINUE_SEARCH;
        break;
    }

    return lDisposition;
}

static bool TryLoadDll(wxString *err)
{
    __try {
        ASIGetNumOfConnectedCameras();
        return true;
    }
    __except (DelayLoadDllExceptionFilter(GetExceptionInformation(), err)) {
        return false;
    }
}

#else // __WINDOWS__

static bool TryLoadDll(wxString *err)
{
    return true;
}

#endif // __WINDOWS__

bool Camera_ZWO::EnumCameras(wxArrayString& names, wxArrayString& ids)
{
    wxString err;
    if (!TryLoadDll(&err))
    {
        wxMessageBox(err, _("Error"), wxOK | wxICON_ERROR);
        return true;
    }

    // Find available cameras
    int numCameras = ASIGetNumOfConnectedCameras();

    for (int i = 0; i < numCameras; i++)
    {
        ASI_CAMERA_INFO info;
        if (ASIGetCameraProperty(&info, i) == ASI_SUCCESS)
        {
            if (numCameras > 1)
                names.Add(wxString::Format("%d: %s", i + 1, info.Name));
            else
                names.Add(info.Name);
            ids.Add(wxString::Format("%d", i));
        }
    }

    return false;
}

bool Camera_ZWO::Connect(const wxString& camId)
{
    wxString err;
    if (!TryLoadDll(&err))
    {
        wxMessageBox(err, _("Error"), wxOK | wxICON_ERROR);
        return true;
    }

    long idx = -1;
    if (camId == DEFAULT_CAMERA_ID)
        idx = 0;
    else
        camId.ToLong(&idx);

    // Find available cameras
    int numCameras = ASIGetNumOfConnectedCameras();

    if (numCameras == 0)
    {
        wxMessageBox(_T("No ZWO cameras detected."), _("Error"), wxOK | wxICON_ERROR);
        return true;
    }

    if (idx < 0 || idx >= numCameras)
    {
        Debug.AddLine(wxString::Format("ZWO: invalid camera id: '%s', ncams = %d", camId, numCameras));
        return true;
    }

    int selected = (int) idx;

    ASI_ERROR_CODE r;
    ASI_CAMERA_INFO info;
    if ((r = ASIGetCameraProperty(&info, selected)) != ASI_SUCCESS)
    {
        Debug.Write(wxString::Format("ASIGetCameraProperty ret %d\n", r));
        wxMessageBox(_("Failed to get camera properties for ZWO ASI Camera."), _("Error"), wxOK | wxICON_ERROR);
        return true;
    }

    if ((r = ASIOpenCamera(selected)) != ASI_SUCCESS)
    {
        Debug.Write(wxString::Format("ASIOpenCamera ret %d\n", r));
        wxMessageBox(_("Failed to open ZWO ASI Camera."), _("Error"), wxOK | wxICON_ERROR);
        return true;
    }

    if ((r = ASIInitCamera(selected)) != ASI_SUCCESS)
    {
        Debug.Write(wxString::Format("ASIInitCamera ret %d\n", r));
        ASICloseCamera(selected);
        wxMessageBox(_("Failed to initizlize ZWO ASI Camera."), _("Error"), wxOK | wxICON_ERROR);
        return true;
    }

    m_cameraId = selected;
    Connected = true;
    Name = info.Name;
    m_isColor = info.IsColorCam != ASI_FALSE;
    Debug.Write(wxString::Format("ZWO: IsColorCam = %d\n", m_isColor));

    int maxBin = 1;
    for (int i = 0; i <= WXSIZEOF(info.SupportedBins); i++)
    {
        if (!info.SupportedBins[i])
            break;
        Debug.Write(wxString::Format("ZWO: supported bin %d = %d\n", i, info.SupportedBins[i]));
        if (info.SupportedBins[i] > maxBin)
            maxBin = info.SupportedBins[i];
    }
    MaxBinning = maxBin;

    if (Binning > MaxBinning)
        Binning = MaxBinning;

    m_maxSize.x = info.MaxWidth;
    m_maxSize.y = info.MaxHeight;

    FullSize.x = m_maxSize.x / Binning;
    FullSize.y = m_maxSize.y / Binning;
    m_prevBinning = Binning;

    delete[] m_buffer;
    m_buffer = new unsigned char[info.MaxWidth * info.MaxHeight];

    m_devicePixelSize = info.PixelSize;

    wxYield();

    int numControls;
    if ((r = ASIGetNumOfControls(m_cameraId, &numControls)) != ASI_SUCCESS)
    {
        Debug.Write(wxString::Format("ASIGetNumOfControls ret %d\n", r));
        Disconnect();
        wxMessageBox(_("Failed to get camera properties for ZWO ASI Camera."), _("Error"), wxOK | wxICON_ERROR);
        return true;
    }

    HasGainControl = false;
    HasCooler = false;

    for (int i = 0; i < numControls; i++)
    {
        ASI_CONTROL_CAPS caps;
        if (ASIGetControlCaps(m_cameraId, i, &caps) == ASI_SUCCESS)
        {
            switch (caps.ControlType)
            {
            case ASI_GAIN:
                if (caps.IsWritable)
                {
                    HasGainControl = true;
                    m_minGain = caps.MinValue;
                    m_maxGain = caps.MaxValue;
                }
                break;
            case ASI_EXPOSURE:
                break;
            case ASI_BANDWIDTHOVERLOAD:
                ASISetControlValue(m_cameraId, ASI_BANDWIDTHOVERLOAD, caps.MinValue, ASI_FALSE);
                break;
            case ASI_HARDWARE_BIN:
                // this control is not present
                break;
            case ASI_COOLER_ON:
                if (caps.IsWritable)
                {
                    Debug.Write("ZWO: camera has cooler\n");
                    HasCooler = true;
                }
                break;
            default:
                break;
            }
        }

    }

    wxYield();

    m_frame = wxRect(FullSize);
    Debug.Write(wxString::Format("ZWO: frame (%d,%d)+(%d,%d)\n", m_frame.x, m_frame.y, m_frame.width, m_frame.height));

    ASISetStartPos(m_cameraId, m_frame.GetLeft(), m_frame.GetTop());
    ASISetROIFormat(m_cameraId, m_frame.GetWidth(), m_frame.GetHeight(), Binning, ASI_IMG_RAW8);

    return false;
}

bool Camera_ZWO::StopCapture(void)
{
    if (m_capturing)
    {
        Debug.AddLine("ZWO: stopcapture");
        ASIStopVideoCapture(m_cameraId);
        m_capturing = false;
    }
    return true;
}

bool Camera_ZWO::Disconnect()
{
    StopCapture();
    ASICloseCamera(m_cameraId);

    Connected = false;

    delete[] m_buffer;
    m_buffer = 0;

    return false;
}

bool Camera_ZWO::GetDevicePixelSize(double* devPixelSize)
{
    if (!Connected)
        return true;

    *devPixelSize = m_devicePixelSize;
    return false;
}

bool Camera_ZWO::SetCoolerOn(bool on)
{
    return (ASISetControlValue(m_cameraId, ASI_COOLER_ON, on ? 1 : 0, ASI_FALSE) != ASI_SUCCESS);
}

bool Camera_ZWO::SetCoolerSetpoint(double temperature)
{
    return (ASISetControlValue(m_cameraId, ASI_TARGET_TEMP, (int) temperature, ASI_FALSE) != ASI_SUCCESS);

}

bool Camera_ZWO::GetCoolerStatus(bool *on, double *setpoint, double *power, double *temperature)
{
    ASI_ERROR_CODE r;
    long value;
    ASI_BOOL isAuto;

    if ((r = ASIGetControlValue(m_cameraId, ASI_COOLER_ON, &value, &isAuto)) != ASI_SUCCESS)
    {
        Debug.Write(wxString::Format("ZWO: error (%d) getting ASI_COOLER_ON\n", r));
        return true;
    }
    *on = value != 0;

    if ((r = ASIGetControlValue(m_cameraId, ASI_TARGET_TEMP, &value, &isAuto)) != ASI_SUCCESS)
    {
        Debug.Write(wxString::Format("ZWO: error (%d) getting ASI_TARGET_TEMP\n", r));
        return true;
    }
    *setpoint = value;

    if ((r = ASIGetControlValue(m_cameraId, ASI_TEMPERATURE, &value, &isAuto)) != ASI_SUCCESS)
    {
        Debug.Write(wxString::Format("ZWO: error (%d) getting ASI_TEMPERATURE\n", r));
        return true;
    }
    *temperature = value / 10.0;

    if ((r = ASIGetControlValue(m_cameraId, ASI_COOLER_POWER_PERC, &value, &isAuto)) != ASI_SUCCESS)
    {
        Debug.Write(wxString::Format("ZWO: error (%d) getting ASI_COOLER_POWER_PERC\n", r));
        return true;
    }
    *power = value;

    return false;
}

inline static int round_down(int v, int m)
{
    return v & ~(m - 1);
}

inline static int round_up(int v, int m)
{
    return round_down(v + m - 1, m);
}

static void flush_buffered_image(int cameraId, usImage& img)
{
    enum { NUM_IMAGE_BUFFERS = 2 }; // camera has 2 internal frame buffers

    // clear buffered frames if any

    for (unsigned int num_cleared = 0; num_cleared < NUM_IMAGE_BUFFERS; num_cleared++)
    {
        ASI_ERROR_CODE status = ASIGetVideoData(cameraId, (unsigned char *) img.ImageData, img.NPixels * sizeof(unsigned short), 0);
        if (status != ASI_SUCCESS)
            break; // no more buffered frames

        Debug.Write(wxString::Format("ZWO: getimagedata clearbuf %u ret %d\n", num_cleared + 1, status));
    }
}

bool Camera_ZWO::Capture(int duration, usImage& img, int options, const wxRect& subframe)
{
    bool binning_change = false;
    if (Binning != m_prevBinning)
    {
        FullSize.x = m_maxSize.x / Binning;
        FullSize.y = m_maxSize.y / Binning;
        m_prevBinning = Binning;
        binning_change = true;
    }

    if (img.Init(FullSize))
    {
        DisconnectWithAlert(CAPT_FAIL_MEMORY);
        return true;
    }

    wxRect frame;
    wxPoint subframePos; // position of subframe within frame

    bool useSubframe = UseSubframes;

    if (subframe.width <= 0 || subframe.height <= 0)
        useSubframe = false;

    if (useSubframe)
    {
        // ensure transfer size is a multiple of 1024
        //  moving the sub-frame or resizing it is somewhat costly (stopCapture / startCapture)

        frame.SetLeft(round_down(subframe.GetLeft(), 32));
        frame.SetRight(round_up(subframe.GetRight() + 1, 32) - 1);
        frame.SetTop(round_down(subframe.GetTop(), 32));
        frame.SetBottom(round_up(subframe.GetBottom() + 1, 32) - 1);

        subframePos = subframe.GetLeftTop() - frame.GetLeftTop();
    }
    else
    {
        frame = wxRect(FullSize);
    }

    long exposureUS = duration * 1000;
    ASI_BOOL tmp;
    long cur_exp;
    if (ASIGetControlValue(m_cameraId, ASI_EXPOSURE, &cur_exp, &tmp) == ASI_SUCCESS &&
        cur_exp != exposureUS)
    {
        Debug.Write(wxString::Format("ZWO: set CONTROL_EXPOSURE %d\n", exposureUS));
        ASISetControlValue(m_cameraId, ASI_EXPOSURE, exposureUS, ASI_FALSE);
    }

    long new_gain = cam_gain(m_minGain, m_maxGain, GuideCameraGain);
    long cur_gain;
    if (ASIGetControlValue(m_cameraId, ASI_GAIN, &cur_gain, &tmp) == ASI_SUCCESS &&
        new_gain != cur_gain)
    {
        Debug.Write(wxString::Format("ZWO: set CONTROL_GAIN %d%% %d\n", GuideCameraGain, new_gain));
        ASISetControlValue(m_cameraId, ASI_GAIN, new_gain, ASI_FALSE);
    }

    bool size_change = frame.GetSize() != m_frame.GetSize();
    bool pos_change = frame.GetLeftTop() != m_frame.GetLeftTop();

    if (size_change || pos_change)
    {
        m_frame = frame;
        Debug.Write(wxString::Format("ZWO: frame (%d,%d)+(%d,%d)\n", m_frame.x, m_frame.y, m_frame.width, m_frame.height));
    }

    if (size_change || binning_change)
    {
        StopCapture();

        ASI_ERROR_CODE status = ASISetROIFormat(m_cameraId, frame.GetWidth(), frame.GetHeight(), Binning, ASI_IMG_RAW8);
        if (status != ASI_SUCCESS)
            Debug.Write(wxString::Format("ZWO: setImageFormat(%d,%d,%hu) => %d\n", frame.GetWidth(), frame.GetHeight(), Binning, status));
    }

    if (pos_change)
    {
        ASI_ERROR_CODE status = ASISetStartPos(m_cameraId, frame.GetLeft(), frame.GetTop());
        if (status != ASI_SUCCESS)
            Debug.Write(wxString::Format("ZWO: setStartPos(%d,%d) => %d\n", frame.GetLeft(), frame.GetTop(), status));
    }

    // the camera and/or driver will buffer frames and return the oldest frame,
    // which could be quite stale. read out all buffered frames so the frame we
    // get is current

    flush_buffered_image(m_cameraId, img);

    if (!m_capturing)
    {
        Debug.AddLine("ZWO: startcapture");
        ASIStartVideoCapture(m_cameraId);
        m_capturing = true;
    }

    int frameSize = frame.GetWidth() * frame.GetHeight();

    int poll = wxMin(duration, 100);

    CameraWatchdog watchdog(duration, duration + GetTimeoutMs() + 10000); // total timeout is 2 * duration + 15s (typically)

    while (true)
    {
        ASI_ERROR_CODE status = ASIGetVideoData(m_cameraId, m_buffer, frameSize, poll);
        if (status == ASI_SUCCESS)
            break;
        if (WorkerThread::InterruptRequested())
        {
            StopCapture();
            return true;
        }
        if (watchdog.Expired())
        {
            Debug.Write(wxString::Format("ZWO: getimagedata ret %d\n", status));
            StopCapture();
            DisconnectWithAlert(CAPT_FAIL_TIMEOUT);
            return true;
        }
    }

    if (useSubframe)
    {
        img.Subframe = subframe;

        // Clear out the image
        img.Clear();

        for (int y = 0; y < subframe.height; y++)
        {
            const unsigned char *src = m_buffer + (y + subframePos.y) * frame.width + subframePos.x;
            unsigned short *dst = img.ImageData + (y + subframe.y) * FullSize.GetWidth() + subframe.x;
            for (int x = 0; x < subframe.width; x++)
                *dst++ = *src++;
        }
    }
    else
    {
        for (int i = 0; i < img.NPixels; i++)
            img.ImageData[i] = m_buffer[i];
    }

    if (options & CAPTURE_SUBTRACT_DARK)
        SubtractDark(img);
    if (m_isColor && Binning == 1 && (options & CAPTURE_RECON))
        QuickLRecon(img);

    return false;
}

inline static ASI_GUIDE_DIRECTION GetASIDirection(int direction)
{
    switch (direction)
    {
    default:
    case NORTH:
        return ASI_GUIDE_NORTH;
    case EAST:
        return ASI_GUIDE_EAST;
    case WEST:
        return ASI_GUIDE_WEST;
    case SOUTH:
        return ASI_GUIDE_SOUTH;
    }
}

bool Camera_ZWO::ST4PulseGuideScope(int direction, int duration)
{
    ASI_GUIDE_DIRECTION d = GetASIDirection(direction);
    ASIPulseGuideOn(m_cameraId, d);
    WorkerThread::MilliSleep(duration, WorkerThread::INT_ANY);
    ASIPulseGuideOff(m_cameraId, d);

    return false;
}

void  Camera_ZWO::ClearGuidePort()
{
    ASIPulseGuideOff(m_cameraId, ASI_GUIDE_NORTH);
    ASIPulseGuideOff(m_cameraId, ASI_GUIDE_SOUTH);
    ASIPulseGuideOff(m_cameraId, ASI_GUIDE_EAST);
    ASIPulseGuideOff(m_cameraId, ASI_GUIDE_WEST);
}

#if defined(__APPLE__)
// workaround link error for missing symbol ___exp10 from libASICamera2.a
#include <math.h>
extern "C" double __exp10(double x) { return pow(10.0, x); }
#endif

#endif // ZWO_ASI
