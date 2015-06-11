#include "gem/supervisor/tbutils/SCurveScan.h"

//#include "gem/supervisor/tbutils/ThresholdEvent.h"
#include "gem/readout/GEMDataParker.h"
#include "gem/readout/GEMDataAMCformat.h"
#include "gem/hw/vfat/HwVFAT2.h"

#include "TH1.h"
#include "TFile.h"
#include "TCanvas.h"
#include <TFrame.h>
#include "TROOT.h"
#include "TString.h"
#include "TError.h"

#include <algorithm>
#include <iomanip>
#include <ctime>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>

#include "cgicc/HTTPRedirectHeader.h"
#include "gem/supervisor/tbutils/VFAT2XMLParser.h"

uint64_t event;

XDAQ_INSTANTIATOR_IMPL(gem::supervisor::tbutils::SCurveScan)

bool First = true;
TH1F* RTime = NULL;
TCanvas *can = NULL;

void gem::supervisor::tbutils::SCurveScan::ConfigParams::registerFields(xdata::Bag<ConfigParams> *bag)
{
    latency      = 12U;
    currentHisto =  0U;

    minThresh    = -60;
    maxThresh    =   0;
    stepSize     = 10U;

    minVcal      =   0;
    maxVcal      = 255;
    stepSizeVcal = 10U;
  
    deviceVT1    = 0x0;
    deviceVT2    =  90;
    deviceVC1    = 0x0;
    deviceVC2    = 255;
  
    bag->addField("currentHisto",     &currentHisto );
    bag->addField("minThresh",        &minThresh    );
    bag->addField("maxThresh",        &maxThresh    );
    bag->addField("stepSize",         &stepSize     );
    bag->addField("minVcal",          &minVcal      );
    bag->addField("maxVcal",          &maxVcal      );
    bag->addField("stepSizeVcal",     &stepSizeVcal );
    bag->addField("deviceVT1",        &deviceVT1    );
    bag->addField("deviceVT2",        &deviceVT2    );
    bag->addField("deviceVC1",        &deviceVC1    );
    bag->addField("deviceVC2",        &deviceVC2    );
}

gem::supervisor::tbutils::SCurveScan::SCurveScan(xdaq::ApplicationStub * s)
  throw (xdaq::exception::Exception) :
  //  xdaq::WebApplication(s),
  gem::supervisor::tbutils::GEMTBUtil(s)
{
  // Detect when the setting of default parameters has been performed
  //SB this->getApplicationInfoSpace()->addListener(this, "urn:xdaq-event:setDefaultValues");

  getApplicationInfoSpace()->fireItemAvailable("scanParams", &scanParams_);
  getApplicationInfoSpace()->fireItemValueRetrieve("scanParams", &scanParams_);

  xgi::framework::deferredbind(this, this, &gem::supervisor::tbutils::SCurveScan::webDefault,      "Default"    );
  xgi::framework::deferredbind(this, this, &gem::supervisor::tbutils::SCurveScan::webConfigure,    "Configure"  );
  xgi::framework::deferredbind(this, this, &gem::supervisor::tbutils::SCurveScan::webStart,        "Start"      );
  runSig_   = toolbox::task::bind(   this, &SCurveScan::run,        "run"       );
  readSig_  = toolbox::task::bind(   this, &SCurveScan::readFIFO,   "readFIFO"  );
  
  wl_ = toolbox::task::getWorkLoopFactory()->getWorkLoop("urn:xdaq-workloop:GEMTestBeamSupervisor:SCurveScan","waiting");
  wl_->activate();

}

gem::supervisor::tbutils::SCurveScan::~SCurveScan()
{
  wl_ = toolbox::task::getWorkLoopFactory()->getWorkLoop("urn:xdaq-workloop:GEMTestBeamSupervisor:SCurveScan","waiting");
  //should we check to see if it's running and try to stop?
  wl_->cancel();
  wl_ = 0;
  
  if (histo) delete histo;
  histo = 0;

  for (int hi = 0; hi < 128; ++hi) {
    if (histos[hi]) delete histos[hi];
    histos[hi] = 0;
  }

  if (outputCanvas) delete outputCanvas;
  outputCanvas = 0;

}

bool gem::supervisor::tbutils::SCurveScan::run(toolbox::task::WorkLoop* wl)
{
    wl_semaphore_.take();
    if (!is_running_) {
      //hw_semaphore_.take();
      //vfatDevice_->setRunMode(1);
      //hw_semaphore_.give();
      //if stop action has killed the run, take the final readout
      wl_semaphore_.give();
      event = 0;
      wl_->submit(readSig_);
      return false;
    }
  
    if (First) {
      First = false;
      if (can) delete can; can = 0;
      can = new TCanvas("can","Dynamic Filling Example",0,0,400,400);
      can->SetFillColor(42);
    }
  
    //send triggers
    hw_semaphore_.take();
    vfatDevice_->setDeviceBaseNode("OptoHybrid.FAST_COM");

    for (size_t trig = 0; trig < 500; ++trig){
      //vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"Send.L1A",0x1);
      //vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"Send.L1ACalPulse",0x1);
      vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"Send.CalPulse",0x1);

    }

    vfatDevice_->setDeviceBaseNode("OptoHybrid.COUNTERS");
    // confParams_.bag.triggersSeen = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"CalPulse.Total");
    confParams_.bag.triggersSeen = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"L1A.External");
    vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
  
    LOG4CPLUS_INFO(getApplicationLogger(),"::run "
        << confParams_.bag.deviceName.toString() 
	<< " ChipID 0x" << hex << confParams_.bag.deviceChipID << dec);

    vfatDevice_->readVFAT2Counters();
    vfatDevice_->setRunMode(0);

    //SB  return true;

    hw_semaphore_.give();
  
    LOG4CPLUS_INFO(getApplicationLogger(),"::run "
        "TriggersSeen " << confParams_.bag.triggersSeen);
  
    if ((uint64_t)(confParams_.bag.triggersSeen) < (uint64_t)(confParams_.bag.nTriggers)) {
  
        hw_semaphore_.take();
        vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
    
        LOG4CPLUS_DEBUG(getApplicationLogger(),"::run "
	    << "not enough triggers, run mode 0x" << std::hex << (unsigned)vfatDevice_->getRunMode() << std::dec);
        
        vfatDevice_->setDeviceBaseNode("GLIB");
        uint32_t bufferDepth = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"LINK1.TRK_FIFO.DEPTH");
        vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
        hw_semaphore_.give();
    
        LOG4CPLUS_INFO(getApplicationLogger(),"::run 1"
	    << " deviceVC2 " << scanParams_.bag.deviceVC2 << " deviceVC1 " << scanParams_.bag.deviceVC1 
            << " minVcal " << scanParams_.bag.minVcal);

        if (bufferDepth < 10) {
            //update
            sleep(0.001);
            hw_semaphore_.take();
      
            //triggersSeen update 
            vfatDevice_->setDeviceBaseNode("OptoHybrid.COUNTERS");
            confParams_.bag.triggersSeen = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"L1A.External");
            vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
      
            LOG4CPLUS_INFO(getApplicationLogger(),"::run "
                << "not enough entries in the buffer, run mode 0x" 
                << std::hex << (unsigned)vfatDevice_->getRunMode() << std::dec);
      
            hw_semaphore_.give();
            wl_semaphore_.give();
            return true;
          }
        else {
            //maybe don't do the readout as a workloop?
            hw_semaphore_.take();
            vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());

            LOG4CPLUS_DEBUG(getApplicationLogger(),"::run "
              "Buffer full, reading out, run mode 0x" << std::hex << (unsigned)vfatDevice_->getRunMode() << std::dec);
       
            hw_semaphore_.give();
            wl_semaphore_.give();
            event = 0;
            wl_->submit(readSig_);
            return true;
        }
        //wl_semaphore_.give();
    }
      else {
    
        hw_semaphore_.take();
        vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
    
        LOG4CPLUS_INFO(getApplicationLogger(),"::run "
            <<"enough triggers, reading out, run mode 0x" 
            << std::hex << (unsigned)vfatDevice_->getRunMode() << std::dec);
    
        vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
        vfatDevice_->setRunMode(0);

        hw_semaphore_.give();
        wl_semaphore_.give();
        event = 0;
        wl_->submit(readSig_);
        wl_semaphore_.take();
        hw_semaphore_.take();
    
        //flush FIFO
        vfatDevice_->setDeviceBaseNode("GLIB.LINK1");
        vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"TRK_FIFO.FLUSH",0x1);
        vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
    
        hw_semaphore_.give();
        
        LOG4CPLUS_INFO(getApplicationLogger(),"::run "
	    << "scan point TriggersSeen " << confParams_.bag.triggersSeen );
    
        LOG4CPLUS_INFO(getApplicationLogger(),"::run 2 "
	    << "deviceVC2 " << scanParams_.bag.deviceVC2 << " deviceVC1 " << scanParams_.bag.deviceVC1 
            << " minVcal " << scanParams_.bag.minVcal << " VC2-VC1 " << (scanParams_.bag.deviceVC2 - scanParams_.bag.deviceVC1) );

        if ( scanParams_.bag.deviceVC2 - scanParams_.bag.deviceVC1 <= scanParams_.bag.stepSizeVcal ) {
    
            //wl_semaphore_.take();
            hw_semaphore_.take();
      
            LOG4CPLUS_INFO(getApplicationLogger(),"::run "
		<< "VC2-VC1 is 0, reading out, run mode 0x" 
                << std::hex << (unsigned)vfatDevice_->getRunMode() << std::dec );
      
            hw_semaphore_.give();
            //SB wl_semaphore_.give();
            wl_->submit(stopSig_);
            return false;
        }
        else if ( (unsigned)scanParams_.bag.deviceVC2 - (unsigned)scanParams_.bag.deviceVC1 > (unsigned)0x0 ) {
    
            hw_semaphore_.take();
      
            vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
      
            LOG4CPLUS_INFO(getApplicationLogger(),"::run " 
                << "VC1= " << scanParams_.bag.deviceVC1 << " VC2-VC1= " << scanParams_.bag.deviceVC2-scanParams_.bag.deviceVC1 
 	        << " maxVcal " << scanParams_.bag.maxVcal); 
      
            LOG4CPLUS_INFO(getApplicationLogger(),"::run "
	        << "VT2-VT1 is > 0x0, run mode 0x" 
                << std::hex << (unsigned)vfatDevice_->getRunMode() << std::dec);
      
            if (scanParams_.bag.deviceVC2 - scanParams_.bag.deviceVC1 > scanParams_.bag.stepSizeVcal) {
              vfatDevice_->setVCal(scanParams_.bag.deviceVC1 + scanParams_.bag.stepSizeVcal);
            }
      
            scanParams_.bag.deviceVT1    = vfatDevice_->getVThreshold1();
            scanParams_.bag.deviceVT2    = vfatDevice_->getVThreshold2();
            confParams_.bag.triggersSeen = 0;
      
            scanParams_.bag.deviceVC1    = vfatDevice_->getVCal();

            LOG4CPLUS_INFO(getApplicationLogger(),"::run "
	        << "new deviceVC1/getVcal " << scanParams_.bag.deviceVC1 );

            vfatDevice_->setDeviceBaseNode("OptoHybrid.COUNTERS.RESETS");
            vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"L1A.External",0x1);
            vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"L1A.Internal",0x1);
            vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"L1A.Delayed",0x1);
            vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"L1A.Total",0x1);
            vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"CalPulse.Total",0x1);
            vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
            vfatDevice_->setRunMode(1);
            vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
      
            LOG4CPLUS_INFO(getApplicationLogger(),"::run " 
		<< "Resubmitting the run workloop, run mode 0x" << std::hex << (unsigned)vfatDevice_->getRunMode() << std::dec);
      
            hw_semaphore_.give();
            wl_semaphore_.give();	
            return true;	
        }
        else {
    
            hw_semaphore_.take();
            vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
      
            LOG4CPLUS_INFO(getApplicationLogger(),"::run 3"
	    << "deviceVC2 " << scanParams_.bag.deviceVC2 << " deviceVC1 " << scanParams_.bag.deviceVC1 
            << " minVcal " << scanParams_.bag.minVcal
	    << " stepSizeVcal " << scanParams_.bag.stepSizeVcal );

            LOG4CPLUS_INFO(getApplicationLogger(),"::run "
                << "reached max Vcal, stopping out, run mode 0x " 
                << std::hex << (unsigned)vfatDevice_->getRunMode() << std::dec);
      
            hw_semaphore_.give();
            wl_semaphore_.give();
            wl_->submit(stopSig_);

            return false;
        }

      } //end if ((uint64_t)(confParams_.bag.triggersSeen)...
  } //end ::run

//might be better done not as a workloop?   confParams_.bag.deviceChipID
bool gem::supervisor::tbutils::SCurveScan::readFIFO(toolbox::task::WorkLoop* wl)
{
    gem::readout::VFATData vfat;
    int ievent=0;
  
    wl_semaphore_.take();
    hw_semaphore_.take();

    // temporrary VFAT11, version 1
    if (confParams_.bag.deviceChipID == (unsigned short)0xfe74 ){
    }
    else {
      LOG4CPLUS_INFO(getApplicationLogger(),"::readFIFO " 
          << confParams_.bag.deviceName.toString() << " ChipID 0x" << hex 
          << confParams_.bag.deviceChipID << dec);
      return false;
    }

    std::string tmpFileName = confParams_.bag.outFileName.toString();
  
    sleep(0.001);
    //read the fifo (x3 times fifo depth), add headers, write to disk, save disk
    boost::format linkForm("LINK%d");
    //should all links have the same fifo depth? if not is this an error?
  
    uint32_t fifoDepth[3];
    //set proper base address
    vfatDevice_->setDeviceBaseNode("GLIB");
    fifoDepth[0] = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),boost::str(linkForm%(link))+".TRK_FIFO.DEPTH");
    fifoDepth[1] = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),boost::str(linkForm%(link))+".TRK_FIFO.DEPTH");
    fifoDepth[2] = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),boost::str(linkForm%(link))+".TRK_FIFO.DEPTH");
    
    //check that the fifos are all the same size?
    int bufferDepth = 0;
    if (fifoDepth[0] != fifoDepth[1] || 
        fifoDepth[0] != fifoDepth[2] || 
        fifoDepth[1] != fifoDepth[2]) {
          LOG4CPLUS_DEBUG(getApplicationLogger(),"::readFIFO "
	      << "tracking data fifos had different depths:: " << fifoDepth[0] << "," << fifoDepth[0] << "," << fifoDepth[0]);
          //use the minimum
          bufferDepth = std::min(fifoDepth[0],std::min(fifoDepth[1],fifoDepth[2]));
    }
    //right now only have FIFO on LINK1
    bufferDepth = fifoDepth[1];
    
    //grab events from the fifo
    bool isFirst = true;
    uint32_t bxNum, bxExp;
  
    while (bufferDepth) {
  
      std::vector<uint32_t> data;
      //readInWords(data);

      vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.TRK_DATA.COL1");
      
      LOG4CPLUS_DEBUG(getApplicationLogger(),"::readFIFO "
	  << "Trying to read register "<<vfatDevice_->getDeviceBaseNode()<<".DATA_RDY");

      if (vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"DATA_RDY")) {

        LOG4CPLUS_DEBUG(getApplicationLogger(),"::readFIFO " 
            <<" Trying to read the block at "<<vfatDevice_->getDeviceBaseNode()<<".DATA");

        //data = vfatDevice_->readBlock("OptoHybrid.GEB.TRK_DATA.COL1.DATA");
        //data = vfatDevice_->readBlock("OptoHybrid.GEB.TRK_DATA.COL1.DATA",7);
        for (int word = 0; word < 7; ++word) {
       	  std::stringstream ss9;
          ss9 << "DATA." << word;
          data.push_back(vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),ss9.str()));
        }
      }
  
      uint32_t TrigReg, bxNumTr;
      uint8_t sBit;
  
      // read trigger data
      vfatDevice_->setDeviceBaseNode("GLIB");
      TrigReg = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"TRG_DATA.DATA");
      bxNumTr = TrigReg >> 6;
      sBit = TrigReg & 0x0000003F;
  
      //if (!checkHeaders(data)) 
      uint16_t b1010, b1100, b1110;
      b1010 = ((data.at(5) & 0xF0000000)>>28);
      b1100 = ((data.at(5) & 0x0000F000)>>12);
      b1110 = ((data.at(4) & 0xF0000000)>>28);
  
      bxNum = data.at(6);
      
      uint16_t bcn, evn, crc, chipid;
      uint64_t msData, lsData;
      uint8_t  flags;
      double   delVC;
  
      if (isFirst)
        bxExp = bxNum;
      
      if (bxNum == bxExp)
        isFirst = false;
      
      bxNum  = data.at(6);
      bcn    = (0x0fff0000 & data.at(5)) >> 16;
      evn    = (0x00000ff0 & data.at(5)) >> 4;
      chipid = (0x0fff0000 & data.at(4)) >> 16;
      flags  = (0x0000000f & data.at(5));
  
      uint64_t data1  = ((0x0000ffff & data.at(4)) << 16) | ((0xffff0000 & data.at(3)) >> 16);
      uint64_t data2  = ((0x0000ffff & data.at(3)) << 16) | ((0xffff0000 & data.at(2)) >> 16);
      uint64_t data3  = ((0x0000ffff & data.at(2)) << 16) | ((0xffff0000 & data.at(1)) >> 16);
      uint64_t data4  = ((0x0000ffff & data.at(1)) << 16) | ((0xffff0000 & data.at(0)) >> 16);
  
      lsData = (data3 << 32) | (data4);
      msData = (data1 << 32) | (data2); 
  
      crc    = 0x0000ffff & data.at(0);
      delVC  = scanParams_.bag.deviceVC1; 
  
      vfat.BC     = ( b1010 << 12 ) | (bcn);                // 1010     | bcn:12
      vfat.EC     = ( b1100 << 12 ) | (evn << 4) | (flags); // 1100     | EC:8      | Flag:4 (zero?)
      vfat.ChipID = ( b1110 << 12 ) | (chipid);             // 1110     | ChipID:12
      vfat.lsData = lsData;                                 // lsData:64
      vfat.msData = msData;                                 // msData:64
      vfat.crc    = crc;                                    // crc:16
  
      /*
      vfat.delVT = delVT;
      vfat.bxNum = (bxNum << 6) | sBit);
      */
      // keepEvent(tmpFileName, ievent, ev, ch);
  
      if (!(((b1010 == 0xa) && (b1100==0xc) && (b1110==0xe)))){
        // dump VFAT data
        LOG4CPLUS_INFO(getApplicationLogger(),"::readFIFO " 
            << "VFAT headers do not match expectation");

        gem::readout::printVFATdataBits(ievent, vfat);
  
        vfatDevice_->setDeviceBaseNode("GLIB");
        bufferDepth = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"LINK1.TRK_FIFO.DEPTH");
        TrigReg = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"TRG_DATA.DATA");
        continue;
      }
  
     /*
      * dump VFAT data */
      gem::readout::printVFATdataBits(ievent, vfat);
      gem::readout::show8bits(sBit); cout << " sBit " << endl; 
  
     /*
      * GEM data filling
      gem::readout::GEMDataParker::fillGEMevent(gem, geb, vfat);
      */
  
      //while (bxNum == bxExp) {
      
      //Maybe add another histogramt that is a combined all channels histogram
      histo->Fill(delVC,(lsData||msData));
  
      //I think it would be nice to time this...
      for (int chan = 0; chan < 128; ++chan) {
        if (chan < 64)
  	  histos[chan]->Fill(delVC,((lsData>>chan))&0x1);
        else
  	  histos[chan]->Fill(delVC,((msData>>(chan-64)))&0x1);
      }
  
      event++; 
      cout << " ::readFIFO: event " << event << endl;

      vfatDevice_->setDeviceBaseNode("GLIB");
      bufferDepth = vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"LINK1.TRK_FIFO.DEPTH");

      if ( event >= nTriggers_ ) {

         vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
         hw_semaphore_.give();
       
         std::string imgRoot = "${XDAQ_DOCUMENT_ROOT}/gemdaq/gemsupervisor/html/images/tbutils/tscan/";
         std::stringstream ss;
         ss << "chanthresh0.png";
         std::string imgName = ss.str();

         outputCanvas->cd();
         outputCanvas->SetFillColor(42);
         outputCanvas->GetFrame()->SetFillColor(21);
      
         histo->Draw("ep0l");
         outputCanvas->Update();
         outputCanvas->SaveAs(TString(imgRoot+imgName));
       
         for (int chan = 0; chan < 128; ++chan) {

           imgRoot = "${XDAQ_DOCUMENT_ROOT}/gemdaq/gemsupervisor/html/images/tbutils/tscan/";
           ss.clear();
           ss.str(std::string());
           ss << "chanthresh" << (chan+1) << ".png";
           imgName = ss.str();
           outputCanvas->cd();
           histos[chan]->Draw("ep0l");
           outputCanvas->Update();
           outputCanvas->SaveAs(TString(imgRoot+imgName));

         }
       
         wl_semaphore_.give();
         return false;
      }
    } // End bufferDepth
  
    hw_semaphore_.give();
    wl_semaphore_.give();
    return false;
  }

void gem::supervisor::tbutils::SCurveScan::scanParameters(xgi::Output *out)
  throw (xgi::exception::Exception)
{
  try {
    std::string isReadonly = "";
    if (is_running_ || is_configured_)
      isReadonly = "readonly";
    *out << cgicc::span()   << std::endl
	 << cgicc::label("Latency").set("for","Latency") << std::endl
	 << cgicc::input().set("id","Latency").set(is_running_?"readonly":"").set("name","Latency")
                          .set("type","number").set("min","0").set("max","255")
                          .set("value",boost::str(boost::format("%d")%static_cast<unsigned>(scanParams_.bag.latency)))
	 << std::endl
	 << cgicc::br() << std::endl

	 << cgicc::label("MinThreshold").set("for","MinThreshold") << std::endl
	 << cgicc::input().set("id","MinThreshold").set(is_running_?"readonly":"").set("name","MinThreshold")
                          .set("type","number").set("min","-255").set("max","255")
                          .set("value",boost::str(boost::format("%d")%(scanParams_.bag.minThresh)))
	 << std::endl

	 << cgicc::label("MaxThreshold").set("for","MaxThreshold") << std::endl
	 << cgicc::input().set("id","MaxThreshold").set(is_running_?"readonly":"").set("name","MaxThreshold")
                          .set("type","number").set("min","-255").set("max","255")
                          .set("value",boost::str(boost::format("%d")%(scanParams_.bag.maxThresh)))
	 << std::endl
	 << cgicc::br() << std::endl

	 << cgicc::label("VStep").set("for","VStep") << std::endl
	 << cgicc::input().set("id","VStep").set(is_running_?"readonly":"").set("name","VStep")
                          .set("type","number").set("min","1").set("max","255")
                          .set("value",boost::str(boost::format("%d")%(scanParams_.bag.stepSize)))
	 << std::endl

	 << cgicc::label("VT1").set("for","VT1") << std::endl
	 << cgicc::input().set("id","VT1").set("name","VT1").set("readonly")
                          .set("value",boost::str(boost::format("%d")%static_cast<unsigned>(scanParams_.bag.deviceVT1)))
	 << std::endl

	 << cgicc::label("VT2").set("for","VT2") << std::endl
	 << cgicc::input().set("id","VT2").set("name","VT2").set("readonly")
                          .set("value",boost::str(boost::format("%d")%static_cast<unsigned>(scanParams_.bag.deviceVT2)))
	 << std::endl
	 << cgicc::br() << std::endl

	 << cgicc::label("MinVcal").set("for","MinVcal") << std::endl
	 << cgicc::input().set("id","MinVcal").set(is_running_?"readonly":"").set("name","MinVcal")
                          .set("type","number").set("min","0").set("max","255")
                          .set("value",boost::str(boost::format("%d")%(scanParams_.bag.minVcal)))
	 << std::endl

	 << cgicc::label("MaxVacl").set("for","MaxVcal") << std::endl
	 << cgicc::input().set("id","MaxVcal").set(is_running_?"readonly":"").set("name","MaxVcal")
                          .set("type","number").set("min","0").set("max","255")
                          .set("value",boost::str(boost::format("%d")%(scanParams_.bag.maxVcal)))
	 << std::endl
	 << cgicc::br() << std::endl

	 << cgicc::label("VcalStep").set("for","VcalStep") << std::endl
	 << cgicc::input().set("id","VcalStep").set(is_running_?"readonly":"").set("name","VcalStep")
                          .set("type","number").set("min","1").set("max","255")
                          .set("value",boost::str(boost::format("%d")%(scanParams_.bag.stepSizeVcal)))
	 << std::endl

	 << cgicc::label("VC1").set("for","VC1") << std::endl
	 << cgicc::input().set("id","VC1").set("name","VC1").set("readonly")
                          .set("value",boost::str(boost::format("%d")%static_cast<unsigned>(scanParams_.bag.deviceVC1)))
	 << std::endl

	 << cgicc::label("VC2").set("for","VC2") << std::endl
	 << cgicc::input().set("id","VC2").set("name","VC2").set("readonly")
                          .set("value",boost::str(boost::format("%d")%static_cast<unsigned>(scanParams_.bag.deviceVC2)))
	 << std::endl
	 << cgicc::br() << std::endl

	 << cgicc::label("NTrigsStep").set("for","NTrigsStep") << std::endl
	 << cgicc::input().set("id","NTrigsStep").set(is_running_?"readonly":"").set("name","NTrigsStep")
                          .set("type","number").set("min","0")
                          .set("value",boost::str(boost::format("%d")%(confParams_.bag.nTriggers)))
	 << cgicc::br() << std::endl
	 << cgicc::label("NTrigsSeen").set("for","NTrigsSeen") << std::endl
	 << cgicc::input().set("id","NTrigsSeen").set("name","NTrigsSeen")
                          .set("type","number").set("min","0").set("readonly")
                          .set("value",boost::str(boost::format("%d")%(confParams_.bag.triggersSeen)))
	 << cgicc::br() << std::endl
	 << cgicc::span()   << std::endl;
  }
  catch (const xgi::exception::Exception& e) {
    LOG4CPLUS_INFO(this->getApplicationLogger(),"Something went wrong displaying VFATS(xgi): " << e.what());
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  catch (const std::exception& e) {
    LOG4CPLUS_INFO(this->getApplicationLogger(),"Something went wrong displaying VFATS(std): " << e.what());
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
}

void gem::supervisor::tbutils::SCurveScan::displayHistograms(xgi::Output *out)
  throw (xgi::exception::Exception)
{
  try {
    *out << cgicc::form().set("method","POST").set("action", "") << std::endl;
    
    *out << cgicc::table().set("class","xdaq-table") << std::endl
	 << cgicc::thead() << std::endl
	 << cgicc::tr()    << std::endl //open
	 << cgicc::th()    << "Select Channel" << cgicc::th() << std::endl
	 << cgicc::th()    << "Histogram"      << cgicc::th() << std::endl
	 << cgicc::tr()    << std::endl //close
	 << cgicc::thead() << std::endl 
      
	 << cgicc::tbody() << std::endl;
    
    *out << cgicc::tr()  << std::endl;
    *out << cgicc::td()
	 << cgicc::label("Channel").set("for","ChannelHist") << std::endl
	 << cgicc::input().set("id","ChannelHist").set("name","ChannelHist")
                          .set("type","number").set("min","0").set("max","128")
                          .set("value",scanParams_.bag.currentHisto.toString())
	 << std::endl
	 << cgicc::br() << std::endl;
    *out << cgicc::input().set("class","button").set("type","button")
                          .set("value","SelectChannel").set("name","DisplayHistogram")
                          .set("onClick","changeImage(this.form)");
    *out << cgicc::td() << std::endl;

    *out << cgicc::td()  << std::endl
	 << cgicc::img().set("src","/gemdaq/gemsupervisor/html/images/tbutils/tscan/chanthresh"+scanParams_.bag.currentHisto.toString()+".png")
                        .set("id","vfatChannelHisto")
	 << cgicc::td()    << std::endl;
    *out << cgicc::tr()    << std::endl
	 << cgicc::tbody() << std::endl
	 << cgicc::table() << std::endl;
    *out << cgicc::form() << cgicc::br() << std::endl;
  }
  catch (const xgi::exception::Exception& e) {
    LOG4CPLUS_INFO(this->getApplicationLogger(),"Something went wrong displaying displayHistograms(xgi): " << e.what());
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  catch (const std::exception& e) {
    LOG4CPLUS_INFO(this->getApplicationLogger(),"Something went wrong displaying displayHistograms(std): " << e.what());
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
}

// HyperDAQ interface
void gem::supervisor::tbutils::SCurveScan::webDefault(xgi::Input *in, xgi::Output *out)
  throw (xgi::exception::Exception)
{
  //LOG4CPLUS_INFO(this->getApplicationLogger(),"gem::supervisor::tbutils::SCurveScan::webDefaul");
  try {
    ////update the page refresh 
    if (!is_working_ && !is_running_) {
    }
    else if (is_working_) {
      cgicc::HTTPResponseHeader &head = out->getHTTPResponseHeader();
      head.addHeader("Refresh","5");
    }
    else if (is_running_) {
      cgicc::HTTPResponseHeader &head = out->getHTTPResponseHeader();
      head.addHeader("Refresh","5");
    }
    
    //generate the control buttons and display the ones that can be touched depending on the run mode
    *out << "<div class=\"xdaq-tab-wrapper\">"            << std::endl;
    *out << "<div class=\"xdaq-tab\" title=\"Control\">"  << std::endl;

    *out << "<table class=\"xdaq-table\">" << std::endl
	 << cgicc::thead() << std::endl
	 << cgicc::tr()    << std::endl //open
	 << cgicc::th()    << "Control" << cgicc::th() << std::endl
	 << cgicc::th()    << "Buffer"  << cgicc::th() << std::endl
	 << cgicc::tr()    << std::endl //close
	 << cgicc::thead() << std::endl 
      
	 << "<tbody>" << std::endl
	 << "<tr>"    << std::endl
	 << "<td>"    << std::endl;
    
    if (!is_initialized_) {
      //have a menu for selecting the VFAT
      *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Initialize") << std::endl;

      selectVFAT(out);
      scanParameters(out);
      
      *out << cgicc::input().set("type", "submit")
	.set("name", "command").set("title", "Initialize hardware acces.")
	.set("value", "Initialize") << std::endl;

      *out << cgicc::form() << std::endl;
    }
    
    else if (!is_configured_) {
      //this will allow the parameters to be set to the chip and scan routine

      *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Configure") << std::endl;
      
      selectVFAT(out);
      scanParameters(out);
      
      //adding aysen's xml parser
      //std::string setConfFile = toolbox::toString("/%s/setConfFile",getApplicationDescriptor()->getURN().c_str());
      //*out << cgicc::form().set("method","POST").set("action",setConfFile) << std::endl ;
      
      *out << cgicc::input().set("type","text").set("name","xmlFilename").set("size","80")
 	                    .set("ENCTYPE","multipart/form-data").set("readonly")
                            .set("value",confParams_.bag.settingsFile.toString()) << std::endl;
      //*out << cgicc::input().set("type","submit").set("value","Set configuration file") << std::endl ;
      //*out << cgicc::form() << std::endl ;
      
      *out << cgicc::br() << std::endl;
      *out << cgicc::input().set("type", "submit")
	.set("name", "command").set("title", "Configure threshold scan.")
	.set("value", "Configure") << std::endl;
      *out << cgicc::form()        << std::endl;
    }
    
    else if (!is_running_) {
      //hardware is initialized and configured, we can start the run
      *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Start") << std::endl;
      
      selectVFAT(out);
      scanParameters(out);
      
      *out << cgicc::input().set("type", "submit")
	.set("name", "command").set("title", "Start threshold scan.")
	.set("value", "Start") << std::endl;
      *out << cgicc::form()    << std::endl;
    }
    
    else if (is_running_) {
      *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Stop") << std::endl;
      
      selectVFAT(out);
      scanParameters(out);
      
      *out << cgicc::input().set("type", "submit")
	.set("name", "command").set("title", "Stop threshold scan.")
	.set("value", "Stop") << std::endl;
      *out << cgicc::form()   << std::endl;
    }
    
    *out << cgicc::comment() << "end the main commands, now putting the halt/reset commands" << cgicc::comment() << cgicc::br() << std::endl;
    *out << cgicc::span()  << std::endl
	 << "<table>" << std::endl
	 << "<tr>"    << std::endl
	 << "<td>"    << std::endl;
      
    //always should have a halt command
    *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Halt") << std::endl;
    
    *out << cgicc::input().set("type", "submit")
      .set("name", "command").set("title", "Halt threshold scan.")
      .set("value", "Halt") << std::endl;
    *out << cgicc::form() << std::endl
	 << "</td>" << std::endl;
    
    *out << "<td>"  << std::endl;
    if (!is_running_) {
      //comand that will take the system to initial and allow to change the hw device
      *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Reset") << std::endl;
      *out << cgicc::input().set("type", "submit")
	.set("name", "command").set("title", "Reset device.")
	.set("value", "Reset") << std::endl;
      *out << cgicc::form() << std::endl;
    }
    *out << "</td>"    << std::endl
	 << "</tr>"    << std::endl
	 << "</table>" << std::endl
	 << cgicc::br() << std::endl
	 << cgicc::span()  << std::endl;

    *out << "</td>" << std::endl;

    *out << "<td>" << std::endl;
    if (is_initialized_)
      showBufferLayout(out);
    *out << "</td>"    << std::endl
	 << "</tr>"    << std::endl
	 << "</tbody>" << std::endl
	 << "</table>" << cgicc::br() << std::endl;
    
    *out << "</div>" << std::endl;
    
    *out << "<div class=\"xdaq-tab\" title=\"Counters\">"  << std::endl;
    if (is_initialized_)
      showCounterLayout(out);
    *out << "</div>" << std::endl;

    *out << "<div class=\"xdaq-tab\" title=\"Fast Commands/Trigger Setup\">"  << std::endl;
    if (is_initialized_)
      fastCommandLayout(out);
    *out << "</div>" << std::endl;

    //place new div class=xdaq-tab here to hold the histograms
    /*
      display a single histogram and have a form that selects which channel you want to display
      use the file name of the histogram that is saved in readFIFO
    */
    *out << "<div class=\"xdaq-tab\" title=\"Channel histograms\">"  << std::endl;
    displayHistograms(out);
    
    *out << "</div>" << std::endl;
    *out << "</div>" << std::endl;
    //</div> //close the new div xdaq-tab

    *out << cgicc::br() << cgicc::br() << std::endl;
    
    //*out << "<div class=\"xdaq-tab\" title=\"Status\">"  << std::endl
    //*out << cgicc::div().set("class","xdaq-tab").set("title","Status")   << std::endl
    *out << "<table class=\"xdaq-table\">" << std::endl
	 << cgicc::thead() << std::endl
	 << cgicc::tr()    << std::endl //open
	 << cgicc::th()    << "Program" << cgicc::th() << std::endl
	 << cgicc::th()    << "System"  << cgicc::th() << std::endl
	 << cgicc::tr()    << std::endl //close
	 << cgicc::thead() << std::endl 
      
	 << "<tbody>" << std::endl
	 << "<tr>"    << std::endl
	 << "<td>"    << std::endl;

    *out << "<table class=\"xdaq-table\">" << std::endl
	 << cgicc::thead() << std::endl
	 << cgicc::tr()    << std::endl //open
	 << cgicc::th()    << "Status" << cgicc::th() << std::endl
	 << cgicc::th()    << "Value"  << cgicc::th() << std::endl
	 << cgicc::tr()    << std::endl //close
	 << cgicc::thead() << std::endl 
      
	 << "<tbody>" << std::endl

	 << "<tr>" << std::endl
	 << "<td>" << "is_working_" << "</td>"
	 << "<td>" << is_working_   << "</td>"
	 << "</tr>"   << std::endl

	 << "<tr>" << std::endl
	 << "<td>" << "is_initialized_" << "</td>"
	 << "<td>" << is_initialized_   << "</td>"
	 << "</tr>"       << std::endl

	 << "<tr>" << std::endl
	 << "<td>" << "is_configured_" << "</td>"
	 << "<td>" << is_configured_   << "</td>"
	 << "</tr>"      << std::endl

	 << "<tr>" << std::endl
	 << "<td>" << "is_running_" << "</td>"
	 << "<td>" << is_running_   << "</td>"
	 << "</tr>"   << std::endl

	 << "</tbody>" << std::endl
	 << "</table>" << cgicc::br() << std::endl
	 << "</td>"    << std::endl;
    
    *out  << "<td>"     << std::endl
	  << "<table class=\"xdaq-table\">" << std::endl
	  << cgicc::thead() << std::endl
	  << cgicc::tr()    << std::endl //open
	  << cgicc::th()    << "Device"     << cgicc::th() << std::endl
	  << cgicc::th()    << "Connected"  << cgicc::th() << std::endl
	  << cgicc::tr()    << std::endl //close
	  << cgicc::thead() << std::endl 
	  << "<tbody>" << std::endl;
    
    if (is_initialized_ && vfatDevice_) {
      hw_semaphore_.take();
      vfatDevice_->setDeviceBaseNode("TEST");
      *out << "<tr>" << std::endl
	   << "<td>" << "GLIB" << "</td>"
	   << "<td>" << vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"GLIB") << "</td>"
	   << "</tr>"   << std::endl
	
	   << "<tr>" << std::endl
	   << "<td>" << "OptoHybrid" << "</td>"
	   << "<td>" << vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"OptoHybrid") << "</td>"
	   << "</tr>"       << std::endl
	
	   << "<tr>" << std::endl
	   << "<td>" << "VFATs" << "</td>"
	   << "<td>" << vfatDevice_->readReg(vfatDevice_->getDeviceBaseNode(),"VFATs") << "</td>"
	   << "</tr>"      << std::endl;
      
      vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
      hw_semaphore_.give();
    }
    
    *out << "</tbody>" << std::endl
	 << "</table>" << std::endl
	 << "</td>"    << std::endl
	 << "</tr>"    << std::endl
	 << "</tbody>" << std::endl
	 << "</table>" << std::endl;
      //<< "</div>"   << std::endl;

    *out << cgicc::script().set("type","text/javascript")
                           .set("src","http://ajax.googleapis.com/ajax/libs/jquery/1/jquery.min.js")
	 << cgicc::script() << std::endl;
    *out << cgicc::script().set("type","text/javascript")
                           .set("src","http://ajax.googleapis.com/ajax/libs/jqueryui/1/jquery-ui.min.js")
	 << cgicc::script() << std::endl;
    *out << cgicc::script().set("type","text/javascript")
                           .set("src","/gemdaq/gemsupervisor/html/scripts/tbutils/changeImage.js")
	 << cgicc::script() << std::endl;
  }
  catch (const xgi::exception::Exception& e) {
    LOG4CPLUS_INFO(this->getApplicationLogger(),"Something went wrong displaying SCurveScan control panel(xgi): " << e.what());
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  catch (const std::exception& e) {
    LOG4CPLUS_INFO(this->getApplicationLogger(),"Something went wrong displaying SCurveScan control panel(std): " << e.what());
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
}


void gem::supervisor::tbutils::SCurveScan::webConfigure(xgi::Input *in, xgi::Output *out)
  throw (xgi::exception::Exception) {

  try {
    cgicc::Cgicc cgi(in);
    
    //aysen's xml parser
    confParams_.bag.settingsFile = cgi.getElement("xmlFilename")->getValue();
    
    cgicc::const_form_iterator element = cgi.getElement("Latency");
    if (element != cgi.getElements().end())
      scanParams_.bag.latency   = element->getIntegerValue();

    element = cgi.getElement("MinThreshold");
    if (element != cgi.getElements().end())
      scanParams_.bag.minThresh = element->getIntegerValue();
    
    element = cgi.getElement("MaxThreshold");
    if (element != cgi.getElements().end())
      scanParams_.bag.maxThresh = element->getIntegerValue();

    element = cgi.getElement("VStep");
    if (element != cgi.getElements().end())
      scanParams_.bag.stepSize  = element->getIntegerValue();
        
    element = cgi.getElement("MinVcal");
    if (element != cgi.getElements().end())
      scanParams_.bag.minVcal = element->getIntegerValue();
    
    element = cgi.getElement("MaxVcal");
    if (element != cgi.getElements().end())
      scanParams_.bag.maxVcal = element->getIntegerValue();

    element = cgi.getElement("VcalStep");
    if (element != cgi.getElements().end())
      scanParams_.bag.stepSizeVcal  = element->getIntegerValue();
        
    element = cgi.getElement("NTrigsStep");
    if (element != cgi.getElements().end())
      confParams_.bag.nTriggers  = element->getIntegerValue();
  }
  catch (const xgi::exception::Exception & e) {
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  catch (const std::exception & e) {
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  
  wl_->submit(confSig_);
  
  redirect(in,out);
}


void gem::supervisor::tbutils::SCurveScan::webStart(xgi::Input *in, xgi::Output *out)
  throw (xgi::exception::Exception) {

  try {
    cgicc::Cgicc cgi(in);
    
    cgicc::const_form_iterator element = cgi.getElement("Latency");
    if (element != cgi.getElements().end())
      scanParams_.bag.latency   = element->getIntegerValue();

    element = cgi.getElement("MinThreshold");
    if (element != cgi.getElements().end())
      scanParams_.bag.minThresh = element->getIntegerValue();
    
    element = cgi.getElement("MaxThreshold");
    if (element != cgi.getElements().end())
      scanParams_.bag.maxThresh = element->getIntegerValue();

    element = cgi.getElement("VStep");
    if (element != cgi.getElements().end())
      scanParams_.bag.stepSize  = element->getIntegerValue();
        
    element = cgi.getElement("MinVcal");
    if (element != cgi.getElements().end())
      scanParams_.bag.minVcal = element->getIntegerValue();
    
    element = cgi.getElement("MaxVcal");
    if (element != cgi.getElements().end())
      scanParams_.bag.maxVcal = element->getIntegerValue();

    element = cgi.getElement("VcalStep");
    if (element != cgi.getElements().end())
      scanParams_.bag.stepSizeVcal  = element->getIntegerValue();
        
    element = cgi.getElement("NTrigsStep");
    if (element != cgi.getElements().end())
      confParams_.bag.nTriggers  = element->getIntegerValue();
  }
  catch (const xgi::exception::Exception & e) {
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  catch (const std::exception & e) {
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  
  wl_->submit(startSig_);
  
  redirect(in,out);
}

// State transitions
    void gem::supervisor::tbutils::SCurveScan::configureAction(toolbox::Event::Reference e)
throw (toolbox::fsm::exception::Exception) {
  
    is_working_ = true;
  
    event = 0;

    latency_       = scanParams_.bag.latency;
    nTriggers_     = confParams_.bag.nTriggers;
    stepSize_      = scanParams_.bag.stepSize;
    minThresh_     = scanParams_.bag.minThresh;
    maxThresh_     = scanParams_.bag.maxThresh;
    stepSizeVcal_  = scanParams_.bag.stepSizeVcal;
    minVcal_       = scanParams_.bag.minVcal;
    maxVcal_       = scanParams_.bag.maxVcal;
    
    hw_semaphore_.take();
    vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());
  
    //make sure device is not running
    vfatDevice_->setRunMode(0);
  
    LOG4CPLUS_INFO(getApplicationLogger(),"loading default settings");
    vfatDevice_->loadDefaults();

   /*
    * default settings for the frontend,
    *  Vcal
    */
    vfatDevice_->setCalPhase(           0);
    scanParams_.bag.calPhase = vfatDevice_->getCalPhase(); 

    vfatDevice_->setVCal(        maxVcal_);
    scanParams_.bag.deviceVC2 = vfatDevice_->getVCal();
    vfatDevice_->setVCal(        minVcal_);
    scanParams_.bag.deviceVC1 = vfatDevice_->getVCal();

    vfatDevice_->setVThreshold1(maxThresh_-minThresh_);
    vfatDevice_->setVThreshold2(max(0,maxThresh_));
    scanParams_.bag.deviceVT1 = vfatDevice_->getVThreshold1();
    scanParams_.bag.deviceVT2 = vfatDevice_->getVThreshold2();
  
    vfatDevice_->setLatency(     latency_);
    scanParams_.bag.latency = vfatDevice_->getLatency();

    is_configured_ = true;
    hw_semaphore_.give();
  
    if (histo) {
      //histo->Delete();
      delete histo;
      histo = 0;
    }
    std::stringstream histName, histTitle;
    histName  << "allchannels";
    histTitle << "Vcal scan for all channels";
    int minTh = scanParams_.bag.minThresh;
    int maxTh = scanParams_.bag.maxThresh;
    int nBins = ((maxTh - minTh) + 1)/(scanParams_.bag.stepSize);
  
    LOG4CPLUS_DEBUG(getApplicationLogger(),"histogram name and title: " << histName.str() 
  		  << ", " << histTitle.str() << "(" << nBins << " bins)");
  
    histo = new TH1F(histName.str().c_str(), histTitle.str().c_str(), nBins, minTh-0.5, maxTh+0.5);
    
    for (unsigned int hi = 0; hi < 128; ++hi) {
      if (histos[hi]) {
        //histos[hi]->Delete();
        delete histos[hi];
        histos[hi] = 0;
      }
      
    histName.clear();
    histName.str(std::string());
    histTitle.clear();
    histTitle.str(std::string());

    histName  << "channel"<<(hi+1);
    histTitle << "Vcal scan for channel "<<(hi+1);
    histos[hi] = new TH1F(histName.str().c_str(), histTitle.str().c_str(), nBins, minTh-0.5, maxTh+0.5);
  }
  outputCanvas = new TCanvas("outputCanvas","outputCanvas",600,600);
  is_working_    = false;
}


    void gem::supervisor::tbutils::SCurveScan::startAction(toolbox::Event::Reference e)
throw (toolbox::fsm::exception::Exception) {
  
  //AppHeader ah;

  is_working_ = true;

  event = 0;

  latency_       = scanParams_.bag.latency;
  nTriggers_     = confParams_.bag.nTriggers;
  stepSize_      = scanParams_.bag.stepSize;
  minThresh_     = scanParams_.bag.minThresh;
  maxThresh_     = scanParams_.bag.maxThresh;
  stepSizeVcal_  = scanParams_.bag.stepSizeVcal;
  minVcal_       = scanParams_.bag.minVcal;
  maxVcal_       = scanParams_.bag.maxVcal;

  time_t now = time(NULL);
  tm *gmtm = gmtime(&now);
  char* utcTime = asctime(gmtm);

  // Output File SCurveSca
  std::string tmpFileName = "SCurveScan_";
  tmpFileName.append(utcTime);
  tmpFileName.erase(std::remove(tmpFileName.begin(), tmpFileName.end(), '\n'), tmpFileName.end());
  tmpFileName.append(".dat");
  std::replace(tmpFileName.begin(), tmpFileName.end(), ' ', '_' );
  std::replace(tmpFileName.begin(), tmpFileName.end(), ':', '-');
  confParams_.bag.outFileName = tmpFileName;

  LOG4CPLUS_DEBUG(getApplicationLogger(),"::startAction " 
      << "Creating file " << confParams_.bag.outFileName.toString());

  std::ofstream scanStream(tmpFileName.c_str(), std::ios::app | std::ios::binary);
  if (scanStream.is_open()){
    LOG4CPLUS_INFO(getApplicationLogger(),"::startAction " 
        << "file " << confParams_.bag.outFileName.toString() << " opened");
  }

  // Setup Scan file, information header
  tmpFileName = "ScanSetup_";
  tmpFileName.append(utcTime);
  tmpFileName.erase(std::remove(tmpFileName.begin(), tmpFileName.end(), '\n'), tmpFileName.end());
  tmpFileName.append(".txt");
  std::replace(tmpFileName.begin(), tmpFileName.end(), ' ', '_' );
  std::replace(tmpFileName.begin(), tmpFileName.end(), ':', '-');
  confParams_.bag.outFileName = tmpFileName;

  LOG4CPLUS_DEBUG(getApplicationLogger(),"::startAction " 
		  << "Created ScanSetup file " << tmpFileName );

  std::ofstream scanSetup(tmpFileName.c_str(), std::ios::app );
  if (scanSetup.is_open()){
    LOG4CPLUS_INFO(getApplicationLogger(),"::startAction " 
        << "file " << tmpFileName << " opened and closed");
    scanSetup << "\n The Time & Date : " << utcTime << endl;
    scanSetup << " SOURCE:       0x1, External " << endl;
    scanSetup << " ChipID        0x" << hex << confParams_.bag.deviceChipID << dec << endl;
    scanSetup << " Latency       " << latency_ << endl;
    scanSetup << " nTriggers     " << nTriggers_  << endl;
    scanSetup << " stepSize      " << stepSize_ << endl;
    scanSetup << " Threshold     " << std::abs(minThresh_) << endl;
    scanSetup << " stepSizeVcal  " << stepSizeVcal_ << endl;
    scanSetup << " minVcal       " << minVcal_ << endl;   
    scanSetup << " maxVcal       " << maxVcal_ << endl;   
    }
  scanSetup.close();

  //char data[128/8]
  is_running_ = true;
  hw_semaphore_.take();

  //set clock source
  /*
  vfatDevice_->setDeviceBaseNode("OptoHybrid.CLOCKING");
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"VFAT.SOURCE",  0x1);
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"CDCE.SOURCE",  0x1);
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"VFAT.FALLBACK",0x1);
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"CDCE.FALLBACK",0x1);
  */

  //set trigger source
  vfatDevice_->setDeviceBaseNode("OptoHybrid.TRIGGER");
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"SOURCE",   0x1);
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"TDC_SBits",(unsigned)confParams_.bag.deviceNum);

  //send resync
  vfatDevice_->setDeviceBaseNode("OptoHybrid.FAST_COM");
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"Send.Resync",0x1);

  //reset counters
  vfatDevice_->setDeviceBaseNode("OptoHybrid.COUNTERS");
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"RESETS.L1A.External",0x1);
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"RESETS.L1A.Internal",0x1);
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"RESETS.L1A.Delayed", 0x1);
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"RESETS.L1A.Total",   0x1);

  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"RESETS.CalPulse.External",0x1);
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"RESETS.CalPulse.Internal",0x1);
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"RESETS.CalPulse.Total",   0x1);

  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"RESETS.Resync",0x1);
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"RESETS.BC0",   0x1);
  
  //flush FIFO
  vfatDevice_->setDeviceBaseNode("GLIB.LINK1");
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"TRK_FIFO.FLUSH", 0x1);
  
  vfatDevice_->setDeviceBaseNode("GLIB");
  vfatDevice_->writeReg(vfatDevice_->getDeviceBaseNode(),"TDC_SBits",(unsigned)confParams_.bag.deviceNum);
  vfatDevice_->setDeviceBaseNode("OptoHybrid.GEB.VFATS."+confParams_.bag.deviceName.toString());

  vfatDevice_->setVThreshold1(maxThresh_-minThresh_);
  vfatDevice_->setVThreshold2(max(0,maxThresh_));
  scanParams_.bag.deviceVT1 = vfatDevice_->getVThreshold1();
  scanParams_.bag.deviceVT2 = vfatDevice_->getVThreshold2();

  scanParams_.bag.latency = vfatDevice_->getLatency();

  vfatDevice_->setRunMode(1);
  hw_semaphore_.give();

  //start readout
  scanStream.close();

  if (histo) {
    //histo->Delete();
    delete histo;
    histo = 0;
  }
  std::stringstream histName, histTitle;
  histName  << "allchannels";
  histTitle << "Vcal scan for all channels";
  int minVc = scanParams_.bag.minVcal;
  int maxVc = scanParams_.bag.maxVcal;
  int nBins = ((maxVc - minVc) + 1)/(scanParams_.bag.stepSizeVcal);

  //write Applicatie  header
  /*
  ah.minVc = minVc;
  ah.maxVc = maxVc;
  ah.stepSizeVcal = scanParams_.bag.stepSizeVcal;
  gem::supervisor::tbutils::keepSCAppHeader(tmpFileName, ah);
  */

  histo = new TH1F(histName.str().c_str(), histTitle.str().c_str(), nBins, minVc-0.5, maxVc+0.5);
  
  for (unsigned int hi = 0; hi < 128; ++hi) {
    if (histos[hi]) {
      delete histos[hi];
      histos[hi] = 0;
    }
    
    histName.clear();
    histName.str(std::string());
    histTitle.clear();
    histTitle.str(std::string());

    histName  << "channel"<<(hi+1);
    histTitle << "Vcal scan for channel "<<(hi+1);
    histos[hi] = new TH1F(histName.str().c_str(), histTitle.str().c_str(), nBins, minVc-0.5, maxVc+0.5);
  }

  //start scan routine
  wl_->submit(runSig_);
  
  is_working_ = false;
}

    void gem::supervisor::tbutils::SCurveScan::resetAction(toolbox::Event::Reference e)
throw (toolbox::fsm::exception::Exception) {

    is_working_ = true;
    gem::supervisor::tbutils::GEMTBUtil::resetAction(e);
    
    scanParams_.bag.latency      = 12U;

    scanParams_.bag.minThresh    = -60;
    scanParams_.bag.maxThresh    =   0;
    scanParams_.bag.stepSize     = 10U;

    scanParams_.bag.minVcal      =   0;
    scanParams_.bag.maxVcal      = 255;
    scanParams_.bag.stepSizeVcal = 10U;
  
    scanParams_.bag.deviceVT1    = 0x0;
    scanParams_.bag.deviceVT2    =  90;
    
    scanParams_.bag.deviceVC1    = 0x0;
    scanParams_.bag.deviceVC2    = 255;

    event = 0;

    is_working_     = false;
}
