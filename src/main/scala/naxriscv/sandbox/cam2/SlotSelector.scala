// SPDX-FileCopyrightText: 2023 "Everybody"
//
// SPDX-License-Identifier: MIT

package naxriscv.sandbox.cam2

import spinal.core._
import spinal.lib._
import spinal.lib.eda.bench.{Bench, Rtl, XilinxStdTargets}

import scala.collection.mutable.ArrayBuffer

case class ScheduleParameter(eventCount : Int,
                             selId : Int){
  val eventBits = log2Up(eventCount)
  val eventType = HardType(UInt(eventBits bits))
}

case class Schedule(p : ScheduleParameter) extends Bundle{
  val event = p.eventType()
}

case class SlotSelectorParameter(slotCount : Int,
                                 eventCount : Int,
                                 selWidth : Int,
                                 schedules : Seq[ScheduleParameter]){

  val eventBits = log2Up(eventCount)
  val eventType = HardType(UInt(eventBits bits))
}

case class SlotSelectorContext(p : SlotSelectorParameter) extends Bundle{
  val event = p.eventType()
  val sel = Bits(p.selWidth bits)
}

case class SlotSelectorIo(p : SlotSelectorParameter) extends Bundle{
  val slots     = Vec.fill(p.slotCount)(slave Stream(SlotSelectorContext(p)))
  val schedules = Vec(p.schedules.map(sp => master Stream(Schedule(sp))))
  val priority = in Bits(p.slotCount bits)
}

class SlotSelector(p : SlotSelectorParameter) extends Component{
  val io = SlotSelectorIo(p)

  val logic = for((schedule, scheduleId) <- io.schedules.zipWithIndex) yield new Area{
    val scheduleParameter = p.schedules(scheduleId)
    val slotsValid = io.slots.map(s => s.valid && s.sel(scheduleParameter.selId)).asBits()
    val doubleMask = slotsValid ## (slotsValid & io.priority)
    val doubleOh = OHMasking.firstV2(doubleMask, firstOrder =  LutInputs.get)
    val selOh = doubleOh.subdivideIn(2 slices).reduce(_ | _)

    schedule.valid := slotsValid.orR

//    val selIdx = OHToUInt(selOh)
//    schedule.event := io.slots.map(_.event).read(selIdx)

    schedule.event := OHToUInt(selOh)
  }

  for((slot, slotIdx) <- io.slots.zipWithIndex){
    slot.ready := logic.map(_.selOh(slotIdx)).orR
  }
}


object SlotSelectorSynthBench extends App{
  LutInputs.set(6)

  val rtls = ArrayBuffer[Rtl]()
  val p = SlotSelectorParameter(
    slotCount   = 32,
    eventCount  = 32,
    selWidth    = 2,
    schedules   = List.tabulate(2)(i => ScheduleParameter(32, i))
  )
  rtls += Rtl(SpinalVerilog(Rtl.ffIo(new SlotSelector(p))))
  val targets = XilinxStdTargets().take(2)

  Bench(rtls, targets)
}

/*
Artix 7 -> 192 Mhz 173 LUT 906 FF
Artix 7 -> 280 Mhz 204 LUT 906 FF
 */