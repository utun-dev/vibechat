#!/usr/bin/env python3
"""Проверить CSV UTUN_MULTIPATH_DIAG и построить автономный HTML с графиками и исходными срезами."""
import argparse
import csv
import json
from pathlib import Path


def read_samples(path):
    """Проверить схему, учёт очередей и интервалы; неправильный CSV не превращать в правдоподобный график."""
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames or len(set(reader.fieldnames)) != len(reader.fieldnames):
            raise ValueError("отсутствуют или повторяются заголовки CSV")
        rows = []
        previous = {}
        for line, raw in enumerate(reader, 2):
            if None in raw or any(value is None for value in raw.values()):
                raise ValueError(f"строка {line}: число значений не соответствует заголовку")
            row = {key: value if key in ("case", "mask", "phase") else int(value) for key, value in raw.items()}
            key = (row["case"], row["mask"], row["side"], row["path"])
            valid = row["dt_us"] > 0 and row["unacked_bytes"] == row["send_bytes"] + row["wait_ack_bytes"]
            valid &= row["send_count"] == row["send_fresh"] + row["send_retries"]
            valid &= row["flight_min"] <= row["flight_bytes"] <= row["flight_max"]
            valid &= row["cwnd_min"] <= row["cwnd_bytes"] <= row["cwnd_max"]
            valid &= row["early_ordinary_ack_entries"] <= row["ack_only_entries"] <= row["ack_entries"]
            valid &= row["early_ordinary_ack_packets"] <= row["ordinary_ack_only_packets"] <= row["ack_only_packets"]
            valid &= row["retries"] <= row["data_packets"] and row["rtt_samples"] <= row["bbr_samples"]
            if key in previous:
                valid &= row["time_us"] - previous[key] == row["dt_us"]
            if not valid:
                raise ValueError(f"строка {line}: нарушен учёт очередей, счётчиков или времени")
            previous[key] = row["time_us"]
            rows.append(row)
    if not rows:
        raise ValueError("CSV не содержит срезов")
    return rows


PAGE = r'''<!doctype html>
<html lang="ru"><meta charset="utf-8"><title>ETCP: очереди и pacing</title>
<style>
body{font:14px system-ui;margin:24px;background:#f5f7fa;color:#182438;max-width:1400px}
h1{font-size:23px}label{display:inline-block;margin:0 16px 12px 0}select,input{font:inherit;padding:4px}
canvas{display:block;width:100%;height:190px;background:white;border:1px solid #dae0e9;border-radius:6px}
section{margin:18px 0}h2{font-size:16px;margin:8px 0}small{color:#526176}.legend{margin:5px 0}
.legend span{margin-right:16px}table{border-collapse:collapse;background:white;width:100%}
td,th{text-align:left;padding:4px 12px;border-bottom:1px solid #e6eaf0}#raw{columns:2;column-gap:30px}
#raw table{break-inside:avoid}#status{font-weight:600;margin:8px 0}button{font:inherit;padding:5px 12px}
</style>
<h1>ETCP: динамика очередей, DATA и ACK</h1>
<p>Источник: <b id="source"></b>. Данные проверены. Наведите курсор на график для просмотра всех полей среза.</p>
<label>Стенд <select id="fixture"></select></label><label>Направление <select id="side"></select></label>
<label>Канал <select id="path"></select></label><label>Фаза <select id="phase"></select></label>
<label>От, с <input id="from" type="number" step="0.1" size="6"></label>
<label>До, с <input id="to" type="number" step="0.1" size="6"></label><button id="reset">Весь интервал</button>
<p><small>Очереди приложения/normalizer/send/wait_ack общие для соединения. Flight, pacer, ACK и канал — для выбранного пути.
Блокировки — число проверок выбора, а не время ожидания. Оранжевый фон: BBR PROBE_RTT.
Пики очереди и задержки эмулятора накоплены с последнего сброса его статистики.</small></p>
<div id="status"></div><div id="charts"></div><h2>Исходный срез</h2><div id="raw"></div>
<script>
const rows=__ROWS__, sourceName=__SOURCE__;
const $=id=>document.getElementById(id), colors=['#1768ac','#dc4e41','#2b8c59','#8856a7','#d08800','#444'];
const rate=(r,k)=>r[k]*8000/r.dt_us, kb=r=>r/1024;
const definitions=[
 ['Очереди: пакеты',[['Приложение',r=>r.app_count],['Фрагменты',r=>r.fragment_count],['send',r=>r.send_count],
  ['wait_ack',r=>r.wait_ack_count],['ACK',r=>r.ack_count]]],
 ['Окно канала: КиБ',[['inflight',r=>kb(r.flight_bytes)],['cwnd',r=>kb(r.cwnd_bytes)],
  ['min flight',r=>kb(r.flight_min)],['max flight',r=>kb(r.flight_max)]]],
 ['Скорость: кбит/с',[['pacing DATA',r=>r.pacing_Bps*8/1000],['DATA TX',r=>rate(r,'data_tx_bytes')],
  ['новые DATA RX',r=>rate(r,'fresh_rx_bytes')],['UDP из эмулятора',r=>rate(r,'wire_sent_bytes')]]],
 ['Pacer: долг и срок готовности, мкс',[['Долг (+) / кредит (−)',r=>r.pacer_debt_us],
  ['До готовности',r=>r.pacer_ready_us],['min debt',r=>r.debt_min_us],['max debt',r=>r.debt_max_us],['Допуск 1мс',r=>1000]]],
 ['ACK: накладные расходы, кбит/с',[['ACK-only',r=>rate(r,'ack_only_wire_bytes')],
  ['ACK внутри DATA',r=>rate(r,'ack_piggy_bytes')]]],
 ['Задержка ACK и RTT: мс',[['ACK-only mean',r=>r.ack_only_entries?r.ack_only_delay_sum_us/r.ack_only_entries/1000:0],
  ['ACK max',r=>r.ack_delay_max_us/1000],['SRTT',r=>r.srtt_us/1000],['min RTT',r=>r.min_rtt_us===4294967295?0:r.min_rtt_us/1000]]],
 ['Наблюдения блокировки выбора: проверки за срез',[['Окно',r=>r.selection_window_blocked],
  ['Pacer',r=>r.selection_pacer_blocked],['ACK-пакеты раньше дедлайна',r=>r.early_ordinary_ack_packets]]],
 ['Эмулятор: ожидающие пакеты',[['Все ожидающие (delay + сериализация + HOLD)',r=>r.channel_pending]]]
];
let current=[], selectedTime=null;
function options(id,values){
 const select=$(id),old=select.value;select.replaceChildren();
 for(const [value,label] of values){const o=document.createElement('option');o.value=value;o.textContent=label;select.append(o);}
 if(values.some(v=>String(v[0])===old))select.value=old;
}
function fixtureRows(){return rows.filter(r=>r.case+'/'+r.mask===$('fixture').value);}
function updateChoices(){
 const fixture=fixtureRows();options('side',[...new Set(fixture.map(r=>r.side))].sort().map(v=>[v,v?'B → A':'A → B']));
 const side=fixture.filter(r=>r.side===Number($('side').value));
 options('path',[...new Set(side.map(r=>r.path))].sort().map(v=>[v,String(v)]));
 options('phase',[['*','Все'],...[...new Set(side.map(r=>r.phase))].map(v=>[v,v])]);
 updateRange();
}
function filtered(){return fixtureRows().filter(r=>r.side===Number($('side').value)&&r.path===Number($('path').value)&&
 ($('phase').value==='*'||r.phase===$('phase').value));}
function updateRange(){
 const v=filtered();$('from').value=v.length?v[0].time_us/1e6:0;$('to').value=v.length?v.at(-1).time_us/1e6:1;draw();
}
function showRaw(row){
 if(!row)return;$('status').textContent=`t=${(row.time_us/1e6).toFixed(6)} с; dt=${row.dt_us} мкс; `+
  `${row.phase}; BBR=${row.bbr_mode}/${row.bbr_cycle}; ${current.length} срезов в диапазоне`;
 const table=document.createElement('table');
 for(const [key,value] of Object.entries(row)){const tr=table.insertRow();tr.insertCell().textContent=key;tr.insertCell().textContent=value;}
 $('raw').replaceChildren(table);
}
function draw(){
 const lo=Number($('from').value)*1e6,hi=Number($('to').value)*1e6;
 current=filtered().filter(r=>r.time_us>=lo&&r.time_us<=hi);$('charts').replaceChildren();
 if(!current.length){$('status').textContent='В выбранном диапазоне нет срезов';$('raw').replaceChildren();return;}
 const first=current[0].time_us,last=Math.max(first+1,current.at(-1).time_us);
 for(const [title,series] of definitions){
  const section=document.createElement('section'),heading=document.createElement('h2');heading.textContent=title;section.append(heading);
  const legend=document.createElement('div');legend.className='legend';
  series.forEach(([label],i)=>{const span=document.createElement('span');span.style.color=colors[i];span.textContent='● '+label;legend.append(span);});
  section.append(legend);const canvas=document.createElement('canvas');section.append(canvas);$('charts').append(section);
  const ratio=window.devicePixelRatio||1,w=canvas.clientWidth,h=190;canvas.width=w*ratio;canvas.height=h*ratio;
  const c=canvas.getContext('2d');c.scale(ratio,ratio);const left=65,right=w-18,top=12,bottom=h-26;
  let ymin=0,ymax=1;for(const r of current)for(const [,value]of series){const v=value(r);ymin=Math.min(ymin,v);ymax=Math.max(ymax,v);}
  if(ymax===ymin)ymax=ymin+1;const x=t=>left+(t-first)/(last-first)*(right-left),y=v=>bottom-(v-ymin)/(ymax-ymin)*(bottom-top);
  for(let i=0;i<current.length;i++)if(current[i].bbr_mode===3){
   c.fillStyle='#fff0d4';c.fillRect(x(current[i].time_us),top,Math.max(1,x(current[i+1]?.time_us||last)-x(current[i].time_us)),bottom-top);
  }
  c.font='11px system-ui';c.fillStyle='#526176';c.strokeStyle='#e3e8ef';
  for(let n=0;n<=4;n++){const value=ymin+(ymax-ymin)*n/4,yy=y(value);c.beginPath();c.moveTo(left,yy);c.lineTo(right,yy);c.stroke();
   c.fillText(value.toFixed(Math.abs(value)<10?2:0),3,yy+4);const t=first+(last-first)*n/4;c.fillText((t/1e6).toFixed(2),x(t)-12,h-6);}
  series.forEach(([,value],i)=>{c.strokeStyle=colors[i];c.lineWidth=i<2?1.8:1;c.beginPath();
   current.forEach((r,n)=>n?c.lineTo(x(r.time_us),y(value(r))):c.moveTo(x(r.time_us),y(value(r))));c.stroke();});
  canvas.onmousemove=event=>{const t=first+(event.offsetX-left)/(right-left)*(last-first);
   const row=current.reduce((a,b)=>Math.abs(a.time_us-t)<Math.abs(b.time_us-t)?a:b);selectedTime=row.time_us;showRaw(row);};
 }
 showRaw(current.reduce((a,b)=>Math.abs(a.time_us-(selectedTime??first))<Math.abs(b.time_us-(selectedTime??first))?a:b));
}
$('source').textContent=sourceName;
options('fixture',[...new Set(rows.map(r=>r.case+'/'+r.mask))].map(v=>[v,v+' (mask)']));
$('fixture').onchange=updateChoices;$('side').onchange=updateChoices;$('path').onchange=updateRange;$('phase').onchange=updateRange;
$('from').onchange=draw;$('to').onchange=draw;$('reset').onclick=updateRange;window.onresize=draw;updateChoices();
</script></html>'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("html", type=Path)
    args = parser.parse_args()
    try:
        rows = read_samples(args.csv)
        page = PAGE.replace("__ROWS__", json.dumps(rows, ensure_ascii=False).replace("<", "\\u003c"))
        page = page.replace("__SOURCE__", json.dumps(args.csv.name, ensure_ascii=False).replace("<", "\\u003c"))
        args.html.write_text(page, encoding="utf-8")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Ошибка отчёта ETCP: {error}\n")
    print(f"Проверено {len(rows)} срезов; отчёт: {args.html}")


if __name__ == "__main__":
    main()
