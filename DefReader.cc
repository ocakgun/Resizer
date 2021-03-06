// Resizer, LEF/DEF gate resizer
// Copyright (c) 2019, Parallax Software, Inc.
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "Machine.hh"
#include "Report.hh"
#include "Error.hh"
#include "PortDirection.hh"
#include "ParseBus.hh"
#include "LefDefNetwork.hh"
#include "defrReader.hpp"

// Use Cadence DEF parser to build ConcreteNetwork based objects.

namespace sta {

static void
registerDefCallbacks();
static int
defDesignCbk(defrCallbackType_e,
	     const char *divider,
	     defiUserData user);
static int
defDividerCbk(defrCallbackType_e,
	      const char *divider,
	      defiUserData user);
static int
defBusBitCbk(defrCallbackType_e,
	     const char *bus_chars,
	     defiUserData user);
static int
defUnitsCbk(defrCallbackType_e,
	    double units,
	    defiUserData user);
static int
defDieAreaCbk(defrCallbackType_e,
	      defiBox *box,
	      defiUserData user);
static int
defComponentCbk(defrCallbackType_e,
		defiComponent *def_component,
		defiUserData user);
static int
defNetCbk(defrCallbackType_e,
	  defiNet *def_net,
	  defiUserData user);
static int
defPinCbk(defrCallbackType_e,
	  defiPin *def_pin,
	  defiUserData user);
static int
defPinEndCbk(defrCallbackType_e,
	     void *,
	     defiUserData user);
static const char *
defToSta(const char *token,
	 Network *network);

////////////////////////////////////////////////////////////////

// DEF parser callback routine state.
class DefReader
{
public:
  DefReader(bool save_def_data,
	    LefDefNetwork *network);
  bool saveDefData() { return save_def_data_; }
  LefDefNetwork *network() { return network_; }

private:
  bool save_def_data_;
  LefDefNetwork *network_;
};

void
readDef(const char *filename,
	bool save_def_data,
	LefDefNetwork *network)
{
  network->setDefFilename(filename);
  // Make top_instance to act as parent to components.
  // Note that top ports are not known yet because PINS section has not been parsed.
  Library *lef_library = network->lefLibrary();
  if (lef_library) {
    defrInitSession();
    registerDefCallbacks();
    DefReader reader(save_def_data, network);
    FILE *stream = fopen(filename, "r");
    if (stream) {
      bool case_sensitive = true;
      defrRead(stream, filename, &reader, case_sensitive);
      defrClear();
      fclose(stream);
    }
    else
      throw FileNotReadable(filename);
  }
  else
    network->report()->printError("Error; no LEF library has been read.\n");
}

static void
registerDefCallbacks()
{
  defrSetDesignCbk(defDesignCbk);
  defrSetDividerCbk(defDividerCbk);
  defrSetBusBitCbk(defBusBitCbk);
  defrSetUnitsCbk(defUnitsCbk);
  defrSetDieAreaCbk(defDieAreaCbk);
  defrSetComponentCbk(defComponentCbk);
  defrSetNetCbk(defNetCbk);
  defrSetPinCbk(defPinCbk);
  defrSetPinEndCbk(defPinEndCbk);
}

DefReader::DefReader(bool save_def_data,
		     LefDefNetwork *network) :
  save_def_data_(save_def_data),
  network_(network)
{
}

#define getDefReader(user) (reinterpret_cast<DefReader *>(user))
#define getNetwork(user) (getDefReader(user)->network())
#define saveDefData(user) (getDefReader(user)->saveDefData())

static void
defError(defiUserData user,
	 const char *fmt, ...)
{
  Report *report = getNetwork(user)->report();
  va_list args;
  va_start(args, fmt);
  report->vprintError(fmt, args);
  va_end(args);
}

static int
defDesignCbk(defrCallbackType_e,
	     const char *design,
	     defiUserData user)
{
  LefDefNetwork *network = getNetwork(user);
  Library *lef_library = network->lefLibrary();
  Cell *top_cell = network->makeCell(lef_library, design, false,
				     network->defFilename());
  Instance *top_instance = network->makeInstance(top_cell, "", nullptr);
  network->setTopInstance(top_instance);
  return 0;
}

static int
defDividerCbk(defrCallbackType_e,
	      const char *divider,
	      defiUserData user)
{
  LefDefNetwork *network = getNetwork(user);
  network->setDivider(divider[0]);
  return 0;
}

static int
defBusBitCbk(defrCallbackType_e,
	     const char *bus_chars,
	     defiUserData user)
{
  LefDefNetwork *network = getNetwork(user);
  Library *lef_lib = network->lefLibrary();
  ConcreteLibrary *lef_clib = reinterpret_cast<ConcreteLibrary*>(lef_lib);
  lef_clib->setBusBrkts(bus_chars[0], bus_chars[1]);
  return 0;
}

static int
defUnitsCbk(defrCallbackType_e,
	    double units,
	    defiUserData user)
{
  LefDefNetwork *network = getNetwork(user);
  network->setDefUnits(units);
  return 0;
}

static int
defDieAreaCbk(defrCallbackType_e,
	      defiBox *box,
	      defiUserData user)
{
  LefDefNetwork *network = getNetwork(user);
  defiPoints points = box->getPoint();
  if (points.numPoints == 2)
    network->setDieArea(points.x[0], points.y[0],
			points.x[1], points.y[1]);
  return 0;
}

static int
defComponentCbk(defrCallbackType_e,
		defiComponent *def_component,
		defiUserData user)
{
  LefDefNetwork *network = getNetwork(user);
  Library *lef_lib = network->lefLibrary();
  const char *def_name = def_component->id();
  const char *sta_name = defToSta(def_name, network);
  const char *macro_name = def_component->name();
  Cell *cell = network->findCell(lef_lib, macro_name);
  if (cell)
    network->makeDefComponent(cell, sta_name,
			      saveDefData(user)
			      ? new defiComponent(*def_component)
			      : nullptr);
  else
    defError(user, "Error: component %s macro %s not found.\n", def_name, macro_name);
  return 0;
}

static int
defPinCbk(defrCallbackType_e,
	  defiPin *def_pin,
	  defiUserData user)
{
  LefDefNetwork *network = getNetwork(user);
  const char *pin_name = def_pin->pinName();
  // Make LEF ports in the top cell.
  // The corresponding top level pins are made when the NET section is parsed.
  Cell *top_cell = network->cell(network->topInstance());
  Port *port = network->makePort(top_cell, pin_name);

  PortDirection *dir = PortDirection::unknown();
  if (def_pin->hasDirection()) {
    const char *def_dir = def_pin->direction();
    if (stringEq(def_dir, "INPUT"))
      dir = PortDirection::input();
    else if (stringEq(def_dir, "OUTPUT"))
      dir = PortDirection::output();
    else if (stringEq(def_dir, "OUTPUT TRISTATE"))
      dir = PortDirection::tristate();
    else if (stringEq(def_dir, "INOUT"))
      dir = PortDirection::bidirect();
  }
  if (def_pin->hasUse()) {
    const char *use = def_pin->use();
    if (stringEq(use, "POWER"))
      dir = PortDirection::power();
    else if (stringEq(use, "GROUND"))
      dir = PortDirection::ground();
  }
  network->setDirection(port, dir);

  if (def_pin->isPlaced() || def_pin->isFixed() || def_pin->isCover())
    network->setLocation(port, DefPt(def_pin->placementX(), def_pin->placementY()));
  return 0;
}

// Finished PINS section so all of the top instance ports are defined.
// Now top_instance::initPins() can be called.
static int
defPinEndCbk(defrCallbackType_e,
	     void *,
	     defiUserData user)
{
  LefDefNetwork *network = getNetwork(user);
  network->initTopInstancePins();
  Cell *top_cell = network->cell(network->topInstance());  
  network->groupBusPorts(top_cell);
  return 0;
}

static int
defNetCbk(defrCallbackType_e,
	  defiNet *def_net,
	  defiUserData user)
{
  LefDefNetwork *network = getNetwork(user);
  const char *def_net_name = def_net->name();
  const char *sta_net_name = defToSta(def_net_name, network);
  Net *net = network->makeNet(sta_net_name, network->topInstance());
  for (int i = 0; i < def_net->numConnections(); i++) {
    const char *def_inst_name = def_net->instance(i);
    const char *sta_inst_name = defToSta(def_inst_name, network);
    const char *pin_name = def_net->pin(i);
    if (stringEq(def_inst_name, "PIN")) {
      Instance *top_inst = network->topInstance();
      Pin *pin = network->findPin(top_inst, pin_name);
      if (pin == nullptr) {
	Cell *cell = network->cell(top_inst);
	Port *port = network->findPort(cell, pin_name);
	if (port)
	  pin = network->makePin(top_inst, port, nullptr);
	else
	  defError(user, "Error: net %s connection to PIN %s not found\n",
		   def_net_name, pin_name);
      }
      if (pin)
	network->makeTerm(pin, net);
    }
    else {
      Instance *top_inst = network->topInstance();
      Instance *inst = network->findChild(top_inst, sta_inst_name);
      if (inst) {
	Cell *cell = network->cell(inst);
	Port *port = network->findPort(cell, pin_name);
	if (port)
	  network->connect(inst, port, net);
	else
	  defError(user, "Error: net %s connection to component %s/%s pin %s not found.\n",
		   def_net_name,
		   def_inst_name,
		   network->name(cell),
		   pin_name);
      }
      else
	defError(user, "Error: net %s connection component %s not found.\n",
	       def_net_name,
	       def_inst_name);
    }
  }
  return 0;
}

// Nada.
// Note that the DEF names violate the normal sta namespace conventions
// because the netlist is flattend.
// Both of the following are valid in DEF and correspond to different Verilog names.
//  h1/r1
//  h1\/r1
static const char *
defToSta(const char *token,
	 Network *)
{
  return token;
}

} // namespace
