using System;
using StepModeSwitch;
class ModeTest {
    sealed class Robot : IModeRobot {
        public bool connected=true,stopped=true,enabled=false,follow=true;
        public int mode=1,calls=0,code=0;
        public bool Connected {get{return connected;}}
        public bool Stopped {get{return stopped;}}
        public bool Enabled {get{return enabled;}}
        public int Mode {get{return mode;}}
        public int SetMode(int target){calls++;if(follow)mode=target;return code;}
    }
    static void Assert(bool value){if(!value)throw new Exception("Mode helper test failed");}
    static void Reject(Robot r,int target){bool failed=false;try{ModeSwitchCore.Switch(r,target,n=>{});}catch{failed=true;}Assert(failed);}
    static void Main(){
        for(int mode=1;mode<=3;mode++){var r=new Robot();ModeSwitchCore.Switch(r,mode,n=>{});Assert(r.mode==mode&&r.calls==(mode==1?0:1));}
        var powered=new Robot{enabled=true};Reject(powered,2);Assert(powered.calls==0);
        var moving=new Robot{stopped=false};Reject(moving,2);Assert(moving.calls==0);
        var lost=new Robot{connected=false};Reject(lost,2);Assert(lost.calls==0);
        var invalid=new Robot();Reject(invalid,4);Assert(invalid.calls==0);
        var rejected=new Robot{code=-1};Reject(rejected,2);Assert(rejected.calls==1);
        var midair=new Robot{follow=false};bool failed=false;
        try{ModeSwitchCore.Switch(midair,2,n=>{midair.enabled=true;});}catch{failed=true;}
        Assert(failed&&midair.calls==1);
        var timeout=new Robot{follow=false};Reject(timeout,3);Assert(timeout.calls==1);
        Console.WriteLine("Mode helper: three targets, rest guards, rejection, state change and missing readback passed.");
    }
}
