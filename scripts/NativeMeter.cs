using System;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
using System.ComponentModel;
using System.Threading;
public static class NativeMeter {
  [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct Startup {
    public int cb; public string reserved,desktop,title;
    public uint x,y,cx,cy,xc,yc,fill,flags; public ushort show,cb2;
    public IntPtr reserved2,stdin,stdout,stderr;
  }
  [StructLayout(LayoutKind.Sequential)] struct PI { public IntPtr process,thread; public uint pid,tid; }
  [StructLayout(LayoutKind.Sequential)] struct Accounting {
    public long user,kernel,periodUser,periodKernel;
    public uint faults,total,active,terminated;
  }
  [StructLayout(LayoutKind.Sequential)] struct BasicLimit {
    public long processTime,jobTime; public uint flags;
    public UIntPtr minWS,maxWS; public uint activeLimit; public UIntPtr affinity; public uint priority,scheduling;
  }
  [StructLayout(LayoutKind.Sequential)] struct IO { public ulong readOps,writeOps,otherOps,readBytes,writeBytes,otherBytes; }
  [StructLayout(LayoutKind.Sequential)] struct Limits {
    public BasicLimit basic; public IO io; public UIntPtr processMemory,jobMemory,peakProcess,peakJob;
  }
  [StructLayout(LayoutKind.Sequential)] struct Memory {
    public uint cb,faults;
    public UIntPtr peakWS,ws,peakPaged,paged,peakNonpaged,nonpaged,pagefile,peakPagefile,privateBytes;
  }
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool CreateProcessW(string exe,StringBuilder cmd,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref Startup si,out PI pi);
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr CreateJobObjectW(IntPtr a,string name);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job,int cls,ref Limits limits,int length);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool QueryInformationJobObject(IntPtr job,int cls,out Accounting data,int length,IntPtr ret);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetHandleInformation(IntPtr handle,uint mask,uint flags);
  [DllImport("kernel32.dll",SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
  [DllImport("kernel32.dll",SetLastError=true)] static extern uint WaitForSingleObject(IntPtr handle,uint ms);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr handle,out uint code);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool TerminateJobObject(IntPtr job,uint code);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessTimes(IntPtr handle,out long created,out long exited,out long kernel,out long user);
  [DllImport("psapi.dll",SetLastError=true)] static extern bool GetProcessMemoryInfo(IntPtr handle,ref Memory data,uint length);
  public class Result {
    public uint exit,pid,job_total_processes;
    public double cpu_seconds_including_children,main_cpu_seconds;
    public ulong main_peak_working_set_bytes,main_peak_pagefile_bytes;
    public int peak_reads; public bool final_peak_read_succeeded;
    public uint active_children_at_main_exit;
    public double job_tail_seconds;
    public double launch_seconds,main_exit_seconds;
    internal IntPtr job,process,thread;
  }
  static void Check(bool ok) { if(!ok)throw new Win32Exception(Marshal.GetLastWin32Error()); }
  public static Result Run(string exe,string arguments,string log,string cwd,int timeoutMS) {
    IntPtr job=CreateJobObjectW(IntPtr.Zero,null);Check(job!=IntPtr.Zero);
    PI pi=new PI();var result=new Result();bool handedOff=false;
    var clock=System.Diagnostics.Stopwatch.StartNew();
    try {
      Limits limits=new Limits();limits.basic.flags=0x2000;
      Check(SetInformationJobObject(job,9,ref limits,Marshal.SizeOf(typeof(Limits))));
      using(var output=new FileStream(log,FileMode.Create,FileAccess.Write,FileShare.Read))
      using(var input=new FileStream("NUL",FileMode.Open,FileAccess.Read,FileShare.ReadWrite)) {
        IntPtr oh=output.SafeFileHandle.DangerousGetHandle(),ih=input.SafeFileHandle.DangerousGetHandle();
        Check(SetHandleInformation(oh,1,1));Check(SetHandleInformation(ih,1,1));
        Startup si=new Startup();si.cb=Marshal.SizeOf(typeof(Startup));si.flags=0x100;si.stdin=ih;si.stdout=oh;si.stderr=oh;
        Check(CreateProcessW(exe,new StringBuilder("\""+exe+"\" "+arguments),IntPtr.Zero,IntPtr.Zero,true,0x08000004,IntPtr.Zero,cwd,ref si,out pi));
        result.pid=pi.pid;Check(AssignProcessToJobObject(job,pi.process));
        if(ResumeThread(pi.thread)==uint.MaxValue)Check(false);
        result.launch_seconds=clock.Elapsed.TotalSeconds;
        while(true) {
          Memory mem=new Memory();mem.cb=(uint)Marshal.SizeOf(typeof(Memory));
          if(GetProcessMemoryInfo(pi.process,ref mem,mem.cb)) {
            result.main_peak_working_set_bytes=Math.Max(result.main_peak_working_set_bytes,mem.peakWS.ToUInt64());
            result.main_peak_pagefile_bytes=Math.Max(result.main_peak_pagefile_bytes,mem.peakPagefile.ToUInt64());result.peak_reads++;
          }
          if(WaitForSingleObject(pi.process,1)==0) {
            result.main_exit_seconds=clock.Elapsed.TotalSeconds;
            result.final_peak_read_succeeded=GetProcessMemoryInfo(pi.process,ref mem,mem.cb);
            if(result.final_peak_read_succeeded) {
              result.main_peak_working_set_bytes=Math.Max(result.main_peak_working_set_bytes,mem.peakWS.ToUInt64());
              result.main_peak_pagefile_bytes=Math.Max(result.main_peak_pagefile_bytes,mem.peakPagefile.ToUInt64());
            }
            break;
          }
          if(clock.ElapsedMilliseconds>timeoutMS)throw new TimeoutException("Yosys timeout");
        }
        Accounting accounting;Check(QueryInformationJobObject(job,1,out accounting,Marshal.SizeOf(typeof(Accounting)),IntPtr.Zero));
        result.active_children_at_main_exit=accounting.active;
        uint code;Check(GetExitCodeProcess(pi.process,out code));result.exit=code;
        long created,exited,kernel,user;Check(GetProcessTimes(pi.process,out created,out exited,out kernel,out user));
        result.main_cpu_seconds=(user+kernel)/1e7;result.cpu_seconds_including_children=(accounting.user+accounting.kernel)/1e7;
        result.job_total_processes=accounting.total;result.job=job;result.process=pi.process;result.thread=pi.thread;handedOff=true;return result;
      }
    } catch { TerminateJobObject(job,124);throw; }
    finally { if(!handedOff){if(pi.thread!=IntPtr.Zero)CloseHandle(pi.thread);if(pi.process!=IntPtr.Zero)CloseHandle(pi.process);CloseHandle(job);} }
  }
  // Descendant accounting and cleanup are outside the official main-process
  // elapsed scope. All descendants finish before the next paired launch.
  public static void Finish(Result result) {
    var clock=System.Diagnostics.Stopwatch.StartNew();
    try {
      Accounting accounting;
      while(true) {
        Check(QueryInformationJobObject(result.job,1,out accounting,Marshal.SizeOf(typeof(Accounting)),IntPtr.Zero));
        if(accounting.active==0)break;
        if(clock.ElapsedMilliseconds>180000)throw new TimeoutException("Descendant accounting timeout");Thread.Sleep(1);
      }
      result.cpu_seconds_including_children=(accounting.user+accounting.kernel)/1e7;result.job_total_processes=accounting.total;
      result.job_tail_seconds=clock.Elapsed.TotalSeconds;
    } finally {CloseHandle(result.thread);CloseHandle(result.process);CloseHandle(result.job);}
  }
}
