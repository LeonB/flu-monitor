// uPlot 1.6.32; keep the chart outside Alpine's reactive proxy.
function burnGraph() {
  let plot, observer, anchor, samples = [], events = [], palette, bounds;
  let chartHost, tooltip, lastHistory;
  const rangeClock = (s) => new Date(s * 1000).toLocaleString([], { month:'short', day:'numeric', hour:'2-digit', minute:'2-digit' });
  const clock = (s) => new Date(s * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  return {
    visibleRange: '',
    init() {
      chartHost = this.$refs.plot;
      tooltip = this.$refs.tooltip;
      observer = new ResizeObserver(() => {
        if (plot && chartHost.clientWidth) plot.setSize({ width: chartHost.clientWidth, height: 340 });
      });
      observer.observe(chartHost);
    },
    destroy() { observer?.disconnect(); plot?.destroy(); plot = null; },
    update(history, reading, settings, loaded, eventDefinitions, receivedMs) {
      if (!loaded || !history.samples?.length) return;
      if (history !== lastHistory) {
        anchor = (receivedMs || Date.now()) / 1000;
        lastHistory = history;
      }
      // Device history uses monotonic ages. Browser time supplies approximate wall-clock labels.
      samples = history.samples.map(s => [anchor - s[0], s[1]]);
      if (reading.thermocouple_ok) {
        const liveX = Date.now() / 1000;
        if (liveX > samples[samples.length - 1][0]) samples.push([liveX, reading.thermocouple_c]);
      }
      const labels = Object.fromEntries((eventDefinitions || []).map(e => [e.slug,e.label]));
      events = (history.events || []).map(e => [anchor - e[0], labels[e[1]] || e[1]]);
      palette = ZONE_ORDER.map(zone => rgbCss(lerpRgb(zoneColor(zone, settings).bg, [255,255,255], .85)));
      const cold = settings.zone_cold_max_c ?? 150, hot = settings.zone_optimal_max_c ?? 280;
      const values = samples.map(s => s[1]);
      const low = Math.min(...values, cold), high = Math.max(...values, hot);
      bounds = { low: low - Math.max(10, (high-low)*.1), high: high + Math.max(30, (high-low)*.15), cold, hot };
      const data = [samples.map(s => s[0]), values];
      if (plot) { plot.setData(data, false); plot.redraw(); return; }
      this.$nextTick(() => {
        if (plot || !chartHost.isConnected || !chartHost.clientWidth) return;
        const css = getComputedStyle(chartHost);
        const text = css.getPropertyValue('--color-text').trim() || '#292720';
        const font = '12px Figtree, sans-serif';
        plot = new uPlot({
          width: chartHost.clientWidth, height: 340,
          legend: { show: false },
          cursor: { drag: { x: true, y: false, setScale: true }, points: { size: 7 } },
          scales: { x: { time: true }, y: { range: () => [bounds.low, bounds.high] } },
          axes: [
            { stroke: text, font, grid: { show: false }, values: (u, ticks) => ticks.map(clock), space: 70 },
            { stroke: text, font, size: 46, grid: { stroke: 'rgba(60,50,35,.08)' }, values: (u, ticks) => ticks.map(v => `${Math.round(v)}°`) }
          ],
          series: [{}, { label: 'Temperature', stroke: text, width: 2.5, points: { show: false }, value: (u,v) => v == null ? '—' : `${v.toFixed(1)} °C` }],
          hooks: {
            drawClear: [u => {
              const { ctx, bbox } = u;
              ctx.save(); ctx.beginPath(); ctx.rect(bbox.left,bbox.top,bbox.width,bbox.height); ctx.clip();
              const levels = [bounds.low, bounds.cold, bounds.hot, bounds.high];
              for (let i=0;i<3;i++) {
                const top = u.valToPos(levels[i+1], 'y', true), bottom = u.valToPos(levels[i], 'y', true);
                ctx.fillStyle = palette[i]; ctx.fillRect(bbox.left,top,bbox.width,bottom-top);
                if (bottom-top > 22*uPlot.pxRatio) {
                  ctx.textAlign='left'; ctx.textBaseline='alphabetic';
                  ctx.fillStyle = text; ctx.font = `${11*uPlot.pxRatio}px Figtree, sans-serif`;
                  const labels = [`Cold below ${bounds.cold}°C`, `Optimal ${bounds.cold}–${bounds.hot}°C`, `Hot above ${bounds.hot}°C`];
                  ctx.fillText(labels[i],bbox.left+8*uPlot.pxRatio,top+15*uPlot.pxRatio);
                }
              }
              ctx.restore();
            }],
            draw: [u => {
              const {ctx,bbox} = u;
              ctx.save(); ctx.beginPath(); ctx.rect(bbox.left,bbox.top,bbox.width,bbox.height); ctx.clip();
              const ratio=uPlot.pxRatio, height=18*ratio, pad=4*ratio;
              ctx.textAlign='left'; ctx.textBaseline='middle';
              const placed=[];
              ctx.font=`${11*ratio}px Figtree, sans-serif`;
              const captions=[
                [bounds.cold,bounds.low,`Cold below ${bounds.cold}°C`],
                [bounds.hot,bounds.cold,`Optimal ${bounds.cold}–${bounds.hot}°C`],
                [bounds.high,bounds.hot,`Hot above ${bounds.hot}°C`]
              ];
              for(const [boundary,bottom,label] of captions) {
                const top=u.valToPos(boundary,'y',true);
                if(u.valToPos(bottom,'y',true)-top>22*ratio)
                  placed.push({x:bbox.left+8*ratio,y:top+3*ratio,w:ctx.measureText(label).width,h:16*ratio});
              }
              ctx.font=`600 ${11*ratio}px Figtree, sans-serif`;
              const overlaps=(a,b) => a.x < b.x+b.w+pad && a.x+a.w+pad > b.x && a.y < b.y+b.h+pad && a.y+a.h+pad > b.y;
              const markers=events.filter(e => e[0]>=u.scales.x.min && e[0]<=u.scales.x.max).map(([time,label]) => {
                let left=samples[0],right=samples[samples.length-1];
                for(let i=1;i<samples.length;i++) if(samples[i][0]>=time) {left=samples[i-1];right=samples[i];break;}
                const f=Math.max(0,Math.min(1,(time-left[0])/(right[0]-left[0] || 1)));
                return {label,x:u.valToPos(time,'x',true),y:u.valToPos(left[1]+f*(right[1]-left[1]),'y',true)};
              });
              for(const {x,y} of markers) {
                ctx.fillStyle='#fffaf1';ctx.strokeStyle=text;ctx.lineWidth=2*ratio;
                ctx.beginPath();ctx.arc(x,y,5*ratio,0,Math.PI*2);ctx.fill();ctx.stroke();
                placed.push({x:x-6*ratio,y:y-6*ratio,w:12*ratio,h:12*ratio});
              }
              for(const {label,x,y} of markers) {
                const width=Math.min(ctx.measureText(label).width+2*pad,bbox.width);
                const labelX=Math.max(bbox.left,Math.min(x-width/2,bbox.left+bbox.width-width));
                let box;
                for (let row=0;row<Math.ceil(bbox.height/height);row++) {
                  for (const direction of [-1,1]) {
                    const labelY=y+direction*(12*ratio+row*(height+pad))-(direction<0?height:0);
                    const candidate={x:labelX,y:labelY,w:width,h:height};
                    if (labelY<bbox.top || labelY+height>bbox.top+bbox.height) continue;
                    if (!placed.some(other=>overlaps(candidate,other))) {box=candidate;break;}
                  }
                  if(box) break;
                }
                // Very dense views keep their markers and cursor readout; labels return as you zoom.
                if(!box) continue;
                placed.push(box);
                ctx.strokeStyle=text; ctx.lineWidth=ratio; ctx.globalAlpha=.35;
                ctx.beginPath(); ctx.moveTo(x,y); ctx.lineTo(Math.max(box.x,Math.min(x,box.x+box.w)),box.y>y?box.y:box.y+box.h); ctx.stroke();
                ctx.globalAlpha=.92; ctx.fillStyle='#fffaf1'; ctx.fillRect(box.x,box.y,box.w,box.h);
                ctx.globalAlpha=1; ctx.fillStyle=text; ctx.fillText(label,box.x+pad,box.y+height/2,box.w-2*pad);
              }
              ctx.restore();
            }],
            setCursor: [u => {
              const idx = u.cursor.idx;
              if (idx == null || u.cursor.left < 0) { tooltip.textContent = 'Move over the graph to inspect a reading'; return; }
              const time = u.data[0][idx], temp = u.data[1][idx];
              const nearby = events.filter(e => Math.abs(e[0]-time) <= 120).map(e => e[1]);
              tooltip.textContent = `${clock(time)} · ${temp.toFixed(1)} °C${nearby.length ? ' · '+nearby.join(', ') : ''}`;
            }],
            setScale: [u => {
              if (!u.scales.x.min || !u.scales.x.max) return;
              this.visibleRange = `${rangeClock(u.scales.x.min)} – ${rangeClock(u.scales.x.max)}`;
            }]
          }
        }, data, chartHost);
        this.resetRange();
        plot.over.addEventListener('dblclick', () => this.resetRange());
        const zoom = (factor, centre) => {
          const lo=plot.scales.x.min, hi=plot.scales.x.max;
          this.applyBounds(centre+(lo-centre)*factor, centre+(hi-centre)*factor);
        };
        plot.over.addEventListener('wheel', e => {
          e.preventDefault();
          const rect=plot.over.getBoundingClientRect();
          zoom(Math.exp(Math.sign(e.deltaY)*.18), plot.posToVal(e.clientX-rect.left,'x'));
        }, {passive:false});
        // Touch: one finger pans; two fingers zoom around their midpoint.
        const pointers=new Map(); let gesture;
        const snapshot=() => {
          const p=[...pointers.values()];
          gesture={p,lo:plot.scales.x.min,hi:plot.scales.x.max};
        };
        plot.over.addEventListener('pointerdown', e => {
          if (e.pointerType==='mouse') return;
          plot.over.setPointerCapture(e.pointerId); pointers.set(e.pointerId,e.clientX); snapshot();
        });
        plot.over.addEventListener('pointermove', e => {
          if (!pointers.has(e.pointerId)) return;
          pointers.set(e.pointerId,e.clientX);
          const p=[...pointers.values()], width=plot.over.clientWidth, span=gesture.hi-gesture.lo;
          if (p.length===1) {
            const shift=(gesture.p[0]-p[0])/width*span;
            this.applyBounds(gesture.lo+shift,gesture.hi+shift);
          } else {
            const before=Math.abs(gesture.p[1]-gesture.p[0]), after=Math.abs(p[1]-p[0]);
            if (before<10 || after<10) return;
            const rect=plot.over.getBoundingClientRect();
            const initialMid=(gesture.p[0]+gesture.p[1])/2-rect.left;
            const currentMid=(p[0]+p[1])/2-rect.left;
            const nextSpan=span*before/after;
            const centre=gesture.lo+initialMid/width*span;
            this.applyBounds(centre-currentMid/width*nextSpan,centre+(1-currentMid/width)*nextSpan);
          }
        });
        const release=e => { pointers.delete(e.pointerId); snapshot(); };
        plot.over.addEventListener('pointerup',release); plot.over.addEventListener('pointercancel',release);
      });
    },
    applyBounds(lo, hi) {
      const max=samples[samples.length-1][0], min=anchor-86400;
      const span=Math.min(86400, Math.max(600,hi-lo));
      lo=Math.max(min,Math.min(lo,max-span)); hi=lo+span;
      plot.setScale('x',{min:lo,max:hi});
    },
    resetRange() {
      if (!plot) return;
      const max=samples[samples.length-1][0];
      plot.setScale('x',{min:max-86400,max});
    }
  };
}
