#include "gem/readout/GEMDataParker.h"
#include "gem/readout/exception/Exception.h"
#include "gem/hw/glib/HwGLIB.h"

#include "gem/utils/soap/GEMSOAPToolBox.h"

#include "toolbox/mem/MemoryPoolFactory.h"
#include "toolbox/mem/CommittedHeapAllocator.h"
#include "i2o/Method.h"
#include "i2o/utils/AddressMap.h"

#include "toolbox/string.h"
#include "xercesc/dom/DOMNode.hpp"
#include "xercesc/dom/DOMNodeList.hpp"
#include "xercesc/util/XercesDefs.hpp"
#include "xcept/tools.h"

#include <boost/utility/binary.hpp>
#include <bitset>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <vector>

#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>

#include "TStopwatch.h"

typedef std::shared_ptr<int*> link_shared_ptr;
typedef gem::readout::GEMDataAMCformat::GEMData  AMCGEMData;
typedef gem::readout::GEMDataAMCformat::GEBData  AMCGEBData;
typedef gem::readout::GEMDataAMCformat::VFATData AMCVFATData;
std::vector<AMCVFATData> vfats;
std::vector<AMCVFATData> erros;

uint16_t gem::readout::GEMslotContents::slot[24] = {
  0xfff,0xfff,0xfff,0xfff,0xfff,0xfff,0xfff,0xfff,
  0xfff,0xfff,0xfff,0xfff,0xfff,0xfff,0xfff,0xfff,
  0xfff,0xfff,0xfff,0xfff,0xfff,0xfff,0xfff,0xfff,
};
bool gem::readout::GEMslotContents::isFileRead = false;

uint32_t kUPDATE = 50000, kUPDATE7 = 7;
int event_ = 0;
int rvent_ = 0;

uint32_t ESexp = -1; 
bool isFirst = true;
uint64_t ZSFlag = 0;

uint32_t contvfats_ = 0;

const int gem::readout::GEMDataParker::I2O_READOUT_NOTIFY=0x84;
const int gem::readout::GEMDataParker::I2O_READOUT_CONFIRM=0x85;

// Main constructor
gem::readout::GEMDataParker::GEMDataParker(gem::hw::glib::HwGLIB& glibDevice,
                                           std::string const& outFileName,
                                           std::string const& errFileName,
                                           std::string const& outputType,
                                           std::string const& slotFileName="slot_table.csv") 
  :
  m_gemLogger(log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("gem:readout:GEMDataParker"))),
  m_queueLock(toolbox::BSem::FULL, true)
{
  //  these bindings necessitate that the GEMDataParker inherit from some xdaq application stuff
  //  i2o::bind(this,&GEMDataParker::onReadoutNotify,I2O_READOUT_NOTIFY,XDAQ_ORGANIZATION_ID);
  //  xoap::bind(this,&GEMDataParker::updateScanParameters,"UpdateScanParameter","urn:GEMDataParker-soap:1");

  glibDevice_   = &glibDevice;
  outFileName_  = outFileName;
  errFileName_  = errFileName;
  slotFileName_ = slotFileName;
  outputType_   = outputType;
  counter_ = {0,0,0,0,0};
  vfat_ = 0;
  event_ = 0;
  rvent_ = 0;
  sumVFAT_ = 0;

  gem::readout::GEMslotContents::initSlots(slotFileName_);
}

uint32_t* gem::readout::GEMDataParker::dumpData(uint8_t const& readout_mask)
{
  uint32_t *point = &counter_[0]; 
  contvfats_ = 0;

  //if [0-7] in deviceNum
  if (readout_mask & 0x1) {
    uint32_t* pDu = gem::readout::GEMDataParker::getGLIBData(0x0, counter_);
    for (unsigned count = 0; count < 5; ++count) counter_[count] = *(pDu+count);
  }
  //if [8-15] in deviceNum
  if (readout_mask & 0x2) {
    uint32_t* pDu = gem::readout::GEMDataParker::getGLIBData(0x1, counter_);
    for (unsigned count = 0; count < 5; ++count) counter_[count] = *(pDu+count);
  }
  //if [16-23] in deviceNum
  if (readout_mask & 0x4) {
    uint32_t* pDu = gem::readout::GEMDataParker::getGLIBData(0x2, counter_); 
    for (unsigned count = 0; count < 5; ++count) counter_[count] = *(pDu+count);
  }

  return point;
}

xoap::MessageReference gem::readout::GEMDataParker::updateScanParameters(xoap::MessageReference msg)
  throw (xoap::exception::Exception)
{
  INFO("GEMDataParker::updateScanParameters()");
  if (msg.isNull()) {
    XCEPT_RAISE(xoap::exception::Exception,"Null message received!");
  }
  
  std::string commandName    = "undefined";
  std::string parameterValue = "-1";
  try {
    std::pair<std::string, std::string> command
      = gem::utils::soap::GEMSOAPToolBox::extractCommandWithParameter(msg);
    commandName = command.first;
    parameterValue = command.second;
    INFO("GEMDataParker received command " << commandName);
  } catch(xoap::exception::Exception& err) {
    std::string msgBase = toolbox::toString("Unable to extract command from CommandWithParameter SOAP message");
    ERROR(toolbox::toString("%s: %s.", msgBase.c_str(), xcept::stdformat_exception_history(err).c_str()));
    XCEPT_DECLARE_NESTED(gem::readout::exception::SOAPCommandParameterProblem, top,
                         toolbox::toString("%s.", msgBase.c_str()), err);
    //p_gemApp->notifyQualified("error", top);
    std::string faultString = toolbox::toString("%s failed", commandName.c_str());
    std::string faultCode   = "Client";
    std::string detail      = toolbox::toString("%s: %s.",
                                                msgBase.c_str(),
                                                err.message().c_str());
    //this has to change to something real, but will come when data parker becomes the gem readout application
    std::string faultActor = "";
    xoap::MessageReference reply =
      gem::utils::soap::GEMSOAPToolBox::makeSOAPFaultReply(faultString, faultCode, detail, faultActor);
    return reply;
  }
  //this has to be injected into the GEM header
  scanParam = std::stoi(parameterValue);
  DEBUG(toolbox::toString("GEMDataParker::updateScanParameters() received command '%s' with value. %s",
                          commandName.c_str(), parameterValue.c_str()));
  gem::utils::soap::GEMSOAPToolBox::makeFSMSOAPReply(commandName, "ParametersUpdated");
}

uint32_t* gem::readout::GEMDataParker::getGLIBData(uint8_t const& link, uint32_t Counter[5])
{
  uint32_t *point = &Counter[0]; 
  TStopwatch timer;

  kUPDATE = 10000;
  ivnt = 0;
  timer.Start();
  Float_t whileStart = (Float_t)timer.RealTime();
  DEBUG(" ::getGLIBData Starting while loop readout " << whileStart
       << std::endl << "FIFO VFAT block depth 0x" << std::hex
       << glibDevice_->getFIFOVFATBlockOccupancy(link)
       << std::endl << "FIFO depth 0x" << std::hex
       << glibDevice_->getFIFOOccupancy(link)
       );
  while ( glibDevice_->getFIFOVFATBlockOccupancy(link) ) {
    //timer.Start();
    Float_t getTrackingStart = (Float_t)timer.RealTime();
    DEBUG(" ::getGLIBData initiating call to getTrackingData(link,"
         << glibDevice_->getFIFOVFATBlockOccupancy(link) << ") "
         << getTrackingStart);
    std::vector<uint32_t> data = glibDevice_->getTrackingData(link,
                                                              glibDevice_->getFIFOVFATBlockOccupancy(link));
    Float_t getTrackingFinish = (Float_t)timer.RealTime();
    DEBUG(" ::getGLIBData The time for one call of getTrackingData(link) " << getTrackingFinish
         << std::endl << "FIFO VFAT block depth 0x" << std::hex
         << glibDevice_->getFIFOVFATBlockOccupancy(link)
         << std::endl << "FIFO depth 0x" << std::hex
         << glibDevice_->getFIFOOccupancy(link)
         );

    uint32_t contqueue = 0;
    for (auto iword = data.begin(); iword != data.end(); ++iword) {
      contqueue++;
      //gem::utils::LockGuard<gem::utils::Lock> guardedLock(m_queueLock);
      DEBUG(" ::getGLIBData pushing into queue 0x"
           << std::setfill('0') << std::setw(8) << std::hex << *iword << std::dec );
      dataque.push(*iword);
      if (contqueue%kUPDATE7 == 0 &&  contqueue != 0) {
        contvfats_++;
	    DEBUG(" ::getGLIBData counter " << contqueue << " contvfats " << contvfats_
             << " dataque.size " << dataque.size());
      }
    }
    DEBUG(" ::getGLIBData end of while loop do we go again?" << std::endl
         << " FIFO VFAT block occupancy  0x" << std::hex << glibDevice_->getFIFOVFATBlockOccupancy(link)
         << std::endl
         << " FIFO occupancy             0x" << std::hex << glibDevice_->getFIFOOccupancy(link) << std::endl
         << " hasTrackingData            0x" << std::hex << glibDevice_->hasTrackingData(link)  << std::endl
         );
  }// while(glibDevice_->getFIFOVFATBlockOccupancy(link))
  timer.Stop();
  Float_t whileFinish = (Float_t)timer.RealTime();
  DEBUG(" ::getGLIBData The time for while loop execution " << whileFinish
       << std::endl
         << " FIFO VFAT block occupancy  0x" << std::hex << glibDevice_->getFIFOVFATBlockOccupancy(link)
         << std::endl
         << " FIFO occupancy             0x" << std::hex << glibDevice_->getFIFOOccupancy(link) << std::endl
         << " hasTrackingData            0x" << std::hex << glibDevice_->hasTrackingData(link)  << std::endl
       );
  return point;
}

uint32_t* gem::readout::GEMDataParker::selectData(uint32_t Counter[5])
{
  uint32_t *point = &Counter[0]; 
  uint32_t* pDQ = gem::readout::GEMDataParker::GEMEventMaker(Counter);
  for (unsigned count = 0; count < 5; ++count) Counter[count] = *(pDQ+count);
  return point;
}

uint32_t* gem::readout::GEMDataParker::GEMEventMaker(uint32_t Counter[5])
{
  uint32_t *point = &Counter[0];

  AMCGEMData  gem; 
  AMCGEBData  geb;
  AMCVFATData vfat;

  int islot = -1;

  // Booking FIFO variables
  uint8_t  ECff;
  uint64_t msVFAT, lsVFAT;
  uint32_t ES;

  if (dataque.empty()) return point;
  DEBUG(" ::GEMEventMaker dataque.size " << dataque.size() );

  this->readVFATblock(dataque);

  uint64_t data1  = dat10 | dat11;
  uint64_t data2  = dat20 | dat21;
  uint64_t data3  = dat30 | dat31;
  uint64_t data4  = dat40 | dat41;

  vfat_++;

  islot = gem::readout::GEMslotContents::GEBslotIndex( (uint32_t)chipid,
                                                       slotFileName_ );

  // GEM Event selector
  ES = ( evn << 12 ) | bcn;
  DEBUG(" ::GEMEventMaker ES 0x" << std::hex << ES << " evn 0x"<< evn << 
          " bcn 0x" << std::hex << bcn << std::dec << " vftas.size " << 
          vfats.size() << " erros.size " << erros.size() << " chip ID 0x" << 
          std::hex << (int)chipid << std::dec << 
        " slot number " << islot << " isFirst " << isFirst << " event " << event_);

  lsVFAT = (data3 << 32) | (data4);
  msVFAT = (data1 << 32) | (data2);

  vfat.BC     = ( b1010 << 12 ) | (bcn);                // 1010     | bcn:12
  vfat.EC     = ( b1100 << 12 ) | (evn << 4) | (flags); // 1100     | EC:8      | Flag:4
  vfat.ChipID = ( b1110 << 12 ) | (chipid);             // 1110     | ChipID:12
  vfat.lsData = lsVFAT;                                 // lsData:64
  vfat.msData = msVFAT;                                 // msData:64
  vfat.BXfrOH = BX;                                     // BXfrOH:32
  vfat.crc    = vfatcrc;                                // crc:16

  if ( ES == ESexp ) { 
    isFirst = false;
  } else { 
    isFirst = true;
    
    if ( vfats.size() != 0 || erros.size() != 0 ) {
      DEBUG(" ::GEMEventMaker isFirst GEMevSelector ");
      gem::readout::GEMDataParker::GEMevSelector(ESexp);
    } 
    
    event_++;
    // VFATS dimensions have limits
    vfats.reserve(MaxVFATS);
    erros.reserve(MaxERRS);
    ESexp = ES;
  }
  DEBUG(" ::GEMEventMaker ES 0x" << std::hex << ES << std::dec << " bool " << isFirst );
  if (islot < 0 || islot > 23) {
    if ( int(erros.size()) <MaxERRS ) erros.push_back(vfat);
    DEBUG(" ::GEMEventMaker warning !!! islot is undefined " << islot << " erros.size " << int(erros.size()) );
  } else {
    // VFATs Pay Load
    if ( int(vfats.size()) <= MaxVFATS ) vfats.push_back(vfat);
    DEBUG(" ::GEMEventMaker event_ " << event_ << " vfats.size " << vfats.size() << std::hex << " ES 0x" << ES << std::dec );
  }//end of event selection 

  Counter[0] = vfat_;
  Counter[1] = event_;
  Counter[2] = vfats.size() + erros.size();
  Counter[3] = vfats.size();
  Counter[4] = erros.size();

return point;
}

void gem::readout::GEMDataParker::GEMevSelector(const  uint32_t& ES)
{
  //  GEM Event Data Format definition
  AMCGEMData  gem; 
  AMCGEBData  geb;
  AMCVFATData vfat;

  DEBUG(" ::GEMEventMaker vfats.size " << int(vfats.size()) << " rvent_ " << rvent_ << " event " << event_);
 
  uint32_t locEvent = 0;
  uint32_t locError = 0;
  std::string TypeDataFlag = "PayLoad";
 
  // contents all local events (one buffer, all links):
  locEvent++;
  uint32_t nChip = 0;
  for (auto iVFAT=vfats.begin(); iVFAT != vfats.end(); ++iVFAT) {//try auto
 
    uint8_t ECff = ( (0x0ff0 & (*iVFAT).EC ) >> 4);
    uint32_t localEvent = ( ECff << 12 ) | ( 0x0fff & (*iVFAT).BC );

    DEBUG(" ::GEMEventMaker vfats ES 0x" << std::hex << ( 0x00ffffff & ES) << " and from vfat 0x" << 
          ( 0x00ffffff & localEvent ) << " EC 0x" << (int)ECff << " BC 0x" << ( 0x0fff & (*iVFAT).BC ) << std::dec );
 
    if ( ES == localEvent ) {
      nChip++;
      // VFATs Pay Load
      geb.vfats.push_back(*iVFAT);
      int islot = gem::readout::GEMslotContents::GEBslotIndex((uint32_t)(*iVFAT).ChipID,
                                                              slotFileName_);
      DEBUG(" ::GEMEventMaker slot number " << islot );
 
      if ( gem::readout::GEMDataParker::VFATfillData( islot, geb) ) {
        if ( vfats.size() == nChip ) {
 
          gem::readout::GEMDataParker::GEMfillHeaders(event_, nChip, gem, geb);
          gem::readout::GEMDataParker::GEMfillTrailers(gem, geb);
          // GEM Event Writing
          DEBUG(" ::GEMEventMaker writing...  geb.vfats.size " << int(geb.vfats.size()) );
          TypeDataFlag = "PayLoad";
          if(int(geb.vfats.size()) != 0) gem::readout::GEMDataParker::writeGEMevent(outFileName_, false, TypeDataFlag,
                                                                                    gem, geb, vfat);
          geb.vfats.clear();
        }// end of writing event
      }// if slot correct
    }// if localEvent
  }// end of GEB PayLoad Data
 
  geb.vfats.clear();
  TypeDataFlag = "Errors";

  // contents all local events (one buffer, all links):
  DEBUG(" ::GEMEventMaker END ES 0x" << std::hex << ES << std::dec << " errES " <<  erros.size() << 
        " rvent_ " << rvent_ );
 
  uint32_t nErro = 0;
  for (auto iErr=erros.begin(); iErr != erros.end(); ++iErr) {
 
    uint8_t ECff = ( (0x0ff0 & (*iErr).EC ) >> 4);
    uint32_t localErr = ( ECff << 12 ) | ( 0x0fff & (*iErr).BC );
    DEBUG(" ::GEMEventMaker ERROR vfats ES 0x" << ES << " EC " << localErr );
  
    if( ES == localErr ) {
      nErro++;
      DEBUG(" ::GEMEventMaker " << " nErro " << nErro << " ES 0x" << std::hex << ES << std::dec );
      // VFATs Errors
      geb.vfats.push_back(*iErr);
      if ( erros.size() == nErro ) {
        // GEMDataAMCformat::printVFATdataBits(nErro, vfat);
        int islot = -1;
        gem::readout::GEMDataParker::VFATfillData( islot, geb);
        gem::readout::GEMDataParker::GEMfillHeaders(rvent_, nErro, gem, geb);
        gem::readout::GEMDataParker::GEMfillTrailers(gem, geb);
        // GEM ERRORS Event Writing
        TypeDataFlag = "Errors";
        if(int(geb.vfats.size()) != 0) gem::readout::GEMDataParker::writeGEMevent(errFileName_, false, TypeDataFlag,
                                                                                  gem, geb, vfat);
        geb.vfats.clear();
      }// if localErr
    }// if localErr
  }// end of GEB PayLoad Data
 
  geb.vfats.clear();
    
  if (event_%kUPDATE == 0 &&  event_ != 0) {
    DEBUG(" ::GEMEventMaker vfats.size " << std::setfill(' ') << std::setw(7) << int(vfats.size()) <<
         " erros.size " << std::setfill(' ') << std::setw(3) << int(erros.size()) << 
         " locEvent   " << std::setfill(' ') << std::setw(6) << locEvent << 
         " locError   " << std::setfill(' ') << std::setw(3) << locError << " event " << event_
         );
  }

  locEvent = 0;
 
  vfats.clear();
  erros.clear();
  // reset event logic
  isFirst = true;
}

bool gem::readout::GEMDataParker::VFATfillData(int const& islot, AMCGEBData&  geb)
{
  // Chamber Header, Zero Suppression flags, Chamber ID
  ZSFlag           = 0x0;                    // :24
  uint64_t ChamID  = 0xdea;                  // :12
  uint64_t sumVFAT = int(geb.vfats.size());  // :28
  geb.header  = (ZSFlag << 40)|(ChamID << 28)|(sumVFAT);
  ZSFlag =  (0xffffff0000000000 & geb.header) >> 40; 
  ChamID =  (0x000000fff0000000 & geb.header) >> 28; 
  sumVFAT=  (0x000000000fffffff & geb.header);    

  DEBUG(" ::VFATfillData ChamID 0x" << ChamID << std::dec << " islot " << islot << " sumVFAT " << sumVFAT);
  
  if (islot == -1) {
    return (false);
  } else {
    ZSFlag           = (ZSFlag | (1 << (23-islot))); // :24
    return (true);
  }// end else
}// end VFATfillData


void gem::readout::GEMDataParker::writeGEMevent(std::string  outFile, bool const&  OKprint,
                                                std::string const& TypeDataFlag,
                                                AMCGEMData&  gem, AMCGEBData&  geb, AMCVFATData& vfat)
{
  if(OKprint) {
    DEBUG(" ::writeGEMevent vfat_ " << vfat_ << " event " << event_ << " sumVFAT " << (0x000000000fffffff & geb.header) <<
          " geb.vfats.size " << int(geb.vfats.size()) );
  }
  // GEM Chamber's Data
  if (outputType_ == "Hex") {
    GEMDataAMCformat::writeGEMhd1 (outFile, event_, gem);
    GEMDataAMCformat::writeGEMhd2 (outFile, event_, gem);
    GEMDataAMCformat::writeGEMhd3 (outFile, event_, gem);
  } else {
    GEMDataAMCformat::writeGEMhd1Binary (outFile, event_, gem);
    GEMDataAMCformat::writeGEMhd2Binary (outFile, event_, gem);
    GEMDataAMCformat::writeGEMhd3Binary (outFile, event_, gem);
  } 
  //  GEB Headers Data
  if (outputType_ == "Hex") {
    GEMDataAMCformat::writeGEBheader (outFile, event_, geb);
    GEMDataAMCformat::writeGEBrunhed (outFile, event_, geb);
  } else {
    GEMDataAMCformat::writeGEBheaderBinary (outFile, event_, geb);
    GEMDataAMCformat::writeGEBrunhedBinary (outFile, event_, geb);
  } // GEMDataAMCformat::printGEBheader (event_, geb);
  //  GEB PayLoad Data
  int nChip=0;
  for (auto iVFAT=geb.vfats.begin(); iVFAT != geb.vfats.end(); ++iVFAT) {
    nChip++;
     
    if (outputType_ == "Hex") {
      GEMDataAMCformat::writeVFATdata (outFile, nChip, *iVFAT); 
    } else {
      GEMDataAMCformat::writeVFATdataBinary (outFile, nChip, *iVFAT);
    };
  }//end of GEB PayLoad Data
  //  GEB Trailers Data
  if (outputType_ == "Hex") {
    GEMDataAMCformat::writeGEBtrailer (outFile, event_, geb);
  } else {
    GEMDataAMCformat::writeGEBtrailerBinary (outFile, event_, geb);
  } 
  //  GEM Trailers Data
  if (outputType_ == "Hex") {
    GEMDataAMCformat::writeGEMtr2 (outFile, event_, gem);
    GEMDataAMCformat::writeGEMtr1 (outFile, event_, gem);
  } else {
    GEMDataAMCformat::writeGEMtr2Binary (outFile, event_, gem);
    GEMDataAMCformat::writeGEMtr1Binary (outFile, event_, gem);
  } 
}

void gem::readout::GEMDataParker::GEMfillHeaders(uint32_t const& event, uint32_t const& BX,
                                                 AMCGEMData& gem, AMCGEBData& geb)
{
  // GEM, All Chamber Data
  // GEM Event Headers [1]
  uint64_t AmcNo       = BOOST_BINARY( 1 );            // :4 
  uint64_t ZeroFlag    = BOOST_BINARY( 0000 );         // :4
  uint64_t LV1ID       = (0x0000000000ffffff & event); // :24
  uint64_t BXID        = 0;                            // :12  ! why we have only 12 Bits for BX !
  uint64_t DataLgth    = BOOST_BINARY( 1 );            // :20

  gem.header1 = (AmcNo <<60)|(ZeroFlag << 56)|(LV1ID << 32)|(BXID << 20)|(DataLgth);

  AmcNo    =  (0xf000000000000000 & gem.header1) >> 60;
  ZeroFlag =  (0x0f00000000000000 & gem.header1) >> 56; 
  LV1ID    =  (0x00ffffff00000000 & gem.header1) >> 32; 
  BXID     =  (0x00000000ffffffff & gem.header1) >> 20;
  DataLgth =  (0x00000000000fffff & gem.header1);

  DEBUG(" ::GEMfillHeaders event " << event << " LV1ID " << LV1ID << " BXID " << BXID);

  // GEM Event Headers [2]
  uint64_t User        = BOOST_BINARY( 1 );    // :32
  uint64_t OrN         = BOOST_BINARY( 1 );    // :16
  uint64_t BoardID     = BOOST_BINARY( 1 );    // :16

  gem.header2 = (User << 32)|(OrN << 16)|(BoardID);

  User     =  (0xffffffff00000000 & gem.header2) >> 32; 
  OrN      =  (0x00000000ffff0000 & gem.header2) >> 16;
  BoardID  =  (0x000000000000ffff & gem.header2);

  // GEM Event Headers [3]
  uint64_t DAVList     = BOOST_BINARY( 1 );    // :24
  uint64_t BufStat     = BOOST_BINARY( 1 );    // :24
  uint64_t DAVCount    = BOOST_BINARY( 1 );    // :5
  uint64_t FormatVer   = BOOST_BINARY( 1 );    // :3
  uint64_t MP7BordStat = BOOST_BINARY( 1 );    // :8

  gem.header3 = (BufStat << 40)|(DAVCount << 16)|(DAVCount << 11)|(FormatVer << 8)|(MP7BordStat);

  DAVList     = (0xffffff0000000000 & gem.header3) >> 40; 
  BufStat     = (0x000000ffffff0000 & gem.header3) >> 16;
  DAVCount    = (0x000000000000ff00 & gem.header3) >> 11;
  FormatVer   = (0x0000000000000f00 & gem.header3) >> 8;
  MP7BordStat = (0x00000000000000ff & gem.header3);

  // RunType:4, all other depends from RunType
  uint64_t RunType = BOOST_BINARY( 1 ); // :4

  //this needs to be populated with dummy values so migration can be made simply
  //scanParam;
  geb.runhed  = (RunType << 60);
}// end GEMfillHeaders

void gem::readout::GEMDataParker::GEMfillTrailers(AMCGEMData&  gem,AMCGEBData&  geb)
{
  // GEM, All Chamber Data
  // GEM Event Treailer [2]
  uint64_t EventStat  = BOOST_BINARY( 1 );    // :32
  uint64_t GEBerrFlag = BOOST_BINARY( 1 );    // :24

  gem.trailer2 = ( EventStat << 40)|(GEBerrFlag);

  // GEM Event Treailer [1]
  uint64_t crc      = BOOST_BINARY( 1 );    // :32
  uint64_t LV1IDT   = BOOST_BINARY( 1 );    // :8
  uint64_t ZeroFlag = BOOST_BINARY( 0000 ); // :4
  uint64_t DataLgth = BOOST_BINARY( 1 );    // :20

  gem.trailer1 = (crc<<32)|(LV1IDT << 24)|(ZeroFlag <<20)|(DataLgth);

  crc      = (0xffffffff00000000 & gem.trailer1) >> 32;
  LV1IDT   = (0x00000000ff000000 & gem.trailer1) >> 24;
  ZeroFlag = (0x0000000000f00000 & gem.trailer1) >> 20;
  DataLgth = (0x00000000000fffff & gem.trailer1);

  // Chamber Trailer, OptoHybrid: crc, wordcount, Chamber status
  uint64_t OHcrc       = BOOST_BINARY( 1 ); // :16
  uint64_t OHwCount    = BOOST_BINARY( 1 ); // :16
  uint64_t ChamStatus  = BOOST_BINARY( 1 ); // :16
  geb.trailer = ((OHcrc << 48)|(OHwCount << 32 )|(ChamStatus << 16));

  OHcrc      = (0xffff000000000000 & geb.trailer) >> 48; 
  OHwCount   = (0x0000ffff00000000 & geb.trailer) >> 32; 
  ChamStatus = (0x00000000ffff0000 & geb.trailer) >> 16;

  DEBUG(" OHcrc 0x" << std::hex << OHcrc << " OHwCount " << OHwCount << " ChamStatus " << ChamStatus << std::dec);
}
void gem::readout::GEMDataParker::readVFATblock(std::queue<uint32_t>& m_dataque)
{
  uint32_t datafront = 0;
  for (int iQue = 0; iQue < 7; iQue++){
    datafront = m_dataque.front();
    DEBUG(" ::GEMEventMaker iQue " << iQue << " 0x"
         << std::setfill('0') << std::setw(8) << std::hex << datafront << std::dec );
    //this never seems to get reset? maybe iQue%7 to read the words after the first block?
    if ((iQue%7) == 5 ) {
      dat41   = ((0xffff0000 & datafront) >> 16 );
      vfatcrc = (0x0000ffff & datafront);
    } else if ( (iQue%7) == 4 ) {
      dat40   = ((0x0000ffff & datafront) << 16 );
      dat31   = ((0xffff0000 & datafront) >> 16 );
    } else if ( (iQue%7) == 3 ) {
      dat21   = ((0xffff0000 & datafront) >> 16 );
      dat30   = ((0x0000ffff & datafront) << 16 );
    } else if ( (iQue%7) == 2 ) {
      dat11   = ((0xffff0000 & datafront) >> 16 );
      dat20   = ((0x0000ffff & datafront) << 16 );
    } else if ( (iQue%7) == 1 ) {
      b1110   = ((0xf0000000 & datafront) >> 28 );
      chipid  = ((0x0fff0000 & datafront) >> 16 );
      dat10   = ((0x0000ffff & datafront) << 16 );
    } else if ( (iQue%7) == 0 ) {
      b1010   = ((0xf0000000 & datafront) >> 28 );
      b1100   = ((0x0000f000 & datafront) >> 12 );
      bcn     = ((0x0fff0000 & datafront) >> 16 );
      evn     = ((0x00000ff0 & datafront) >>  4 );
      flags   = (0x0000000f & datafront);
      
      if (!(b1010 == 0xa && b1100 == 0xc)) {
        bool misAligned_ = true;
        while ((misAligned_) && (m_dataque.size()>7)){
          /* we have a misaligned word, increment misalignment counter, pop queue,
             push bad value into some form of storage for later analysis?
             then continue with the loop, but without incrementing iQue so we hopefully
             eventually align again
             
             Do not go through all the loop with condition statements 
             since iQue stays the same and we removed 7 blocks
             -MD
          */
          INFO(" ::GEMEventMaker found misaligned word 0x"
               << std::setfill('0') << std::hex << datafront << std::dec
               << " queue dataque.size " << m_dataque.size() );
          m_dataque.pop();
          datafront = m_dataque.front();
          b1010   = ((0xf0000000 & datafront) >> 28 );
          b1100   = ((0x0000f000 & datafront) >> 12 );
          bcn     = ((0x0fff0000 & datafront) >> 16 );
          evn     = ((0x00000ff0 & datafront) >>  4 );
          flags   = (0x0000000f & datafront);
          if ((b1010 == 0xa && b1100 == 0xc)) { misAligned_ = false;}
        }// end of while misaligned
      }
    } else if ( (iQue%7) == 6 ) {
      BX      = datafront;
    }
    DEBUG(" ::GEMEventMaker (pre pop) dataque.size " << m_dataque.size() );
    m_dataque.pop();
    DEBUG(" ::GEMEventMaker (post pop)  dataque.size " << m_dataque.size() );
  }// end queue
}
