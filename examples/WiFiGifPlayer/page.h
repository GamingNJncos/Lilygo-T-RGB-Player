#pragma once
#include <pgmspace.h>

static const char HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-RGB Player</title>
<style>
*{box-sizing:border-box}body{font-family:system-ui,sans-serif;background:#0d1117;color:#e6edf3;max-width:540px;margin:0 auto;padding:16px}
h1{text-align:center;color:#ff6b6b;margin:12px 0 4px}
.card{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:16px;margin:10px 0}
.card h2{color:#ff6b6b;font-size:1em;margin:0 0 10px}
input[type=file]{display:block;margin:6px 0;font-size:.9em}
label{display:block;margin-top:6px;font-size:.82em;color:#8b949e}
input[type=number],input[type=text]{background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:4px;padding:5px;width:100%}
input[type=color]{width:100%;height:32px;border:1px solid #30363d;border-radius:4px;cursor:pointer;background:#0d1117}
.row{display:flex;gap:8px}.row>div{flex:1}
button{background:#ff6b6b;color:#fff;border:none;border-radius:6px;padding:9px 18px;cursor:pointer;font-size:.92em;margin-top:8px}
button:hover{background:#e05555}button.alt{background:#30363d}button.alt:hover{background:#444c56}
.bar{background:#0d1117;border-radius:4px;height:16px;margin:6px 0;overflow:hidden}
.fill{background:#ff6b6b;height:100%;width:0%;transition:width .2s;font-size:10px;line-height:16px;text-align:center;color:#fff}
#msg{text-align:center;padding:6px;color:#7ee787;font-weight:600;min-height:20px;font-size:.9em}
#lib{margin-top:8px;font-size:.85em}
#lib a{color:#58a6ff;cursor:pointer;display:block;padding:4px 0}
canvas{display:none}
.ctrl{display:flex;gap:8px;justify-content:center}
</style></head><body>
<h1>T-RGB Player</h1>
<div id="msg">Ready</div>

<div class="card">
<h2>GIF Upload</h2>
<input type="file" id="gf" accept=".gif">
<label>Name (optional)</label>
<input type="text" id="gn" placeholder="my_animation">
<div class="bar"><div class="fill" id="gb"></div></div>
<button onclick="sendGif()">Upload &amp; Play</button>
</div>

<div class="card">
<h2>Sprite Sheet / Image</h2>
<p style="font-size:.8em;color:#8b949e;margin:0 0 8px">Any image format. Sprites auto-detected from transparency. No transparency = single frame.</p>
<input type="file" id="sf" accept="image/*">
<label>Name (optional)</label>
<input type="text" id="sn" placeholder="my_sprite">
<div class="row">
<div><label>Delay ms</label><input type="number" id="sd" value="100" min="16" max="5000"></div>
<div><label>Min sprite px</label><input type="number" id="smin" value="16" min="4" max="200"></div>
</div>
<div class="bar"><div class="fill" id="sb"></div></div>
<button onclick="sendSprite()">Auto-Detect &amp; Play</button>
</div>

<div class="card">
<h2>Display Settings</h2>
<div class="row"><div><label>Background</label><br><input type="color" id="bgc" value="#000000"></div><div><button onclick="fetch('/bg?c='+document.getElementById('bgc').value.substring(1)).then(function(){M('Background set')})">Apply</button></div></div>
<label>Zoom margin (px from edge)</label>
<div class="row"><div style="flex:1"><input type="range" id="zoomSlider" min="1" max="140" value="20" style="width:100%" oninput="document.getElementById('zoomVal').textContent=this.value+'px'"></div><div><span id="zoomVal" style="font-size:.85em;color:#8b949e">20px</span></div><div><button class="alt" onclick="fetch('/zoom?t='+(480-document.getElementById('zoomSlider').value*2)).then(function(){M('Zoom margin set')})">Apply</button></div></div>
<div style="margin-top:8px"><label>Weather overlay</label>
<div class="row"><div><button id="wxBtn" onclick="fetch('/wxoverlay?on='+(document.getElementById('wxBtn').textContent==='Enable'?'1':'0')).then(function(r){return r.json()}).then(function(d){document.getElementById('wxBtn').textContent=d.on?'Disable':'Enable';M('Weather overlay '+(d.on?'on':'off'))})">Enable</button></div><div><select id="wxPos" onchange="fetch('/wxoverlay?pos='+this.value)"><option value="0">Top-Left</option><option value="1">Top-Right</option><option value="2">Bottom-Left</option><option value="3">Bottom-Right</option></select></div></div></div>
</div>

<div class="card">
<h2>Display</h2>
<div class="ctrl">
<button class="alt" onclick="grabScreen()">Screenshot</button>
<button class="alt" id="liveBtn" onclick="toggleLive()">Live View</button>
</div>
<div style="text-align:center;margin-top:6px"><img id="screenImg" style="max-width:240px;border-radius:50%;border:2px solid #30363d;display:none"></div>
</div>

<div class="card">
<h2>Library</h2>
<div class="ctrl">
<button class="alt" onclick="fetch('/ctrl?a=prev').then(r=>r.text()).then(M)">Prev</button>
<button class="alt" onclick="fetch('/ctrl?a=pause').then(r=>r.text()).then(M)">Pause</button>
<button class="alt" onclick="fetch('/ctrl?a=next').then(r=>r.text()).then(M)">Next</button>
<button class="alt" onclick="fetch('/stop').then(r=>r.text()).then(M)">Stop</button>
</div>
<div id="lib">Loading...</div>
<button class="alt" onclick="loadLib()">Refresh</button>
</div>

<div class="card">
<h2>File Manager</h2>
<div id="files">Loading...</div>
<button class="alt" onclick="loadFiles()">Refresh</button>
<a href="/browse" style="display:inline-block;margin-top:6px;color:#58a6ff;font-size:.85em">Browse SD Card</a>
</div>

<canvas id="cv"></canvas>
<script>
var MAX_FRAMES=256;
var M=function(t){document.getElementById('msg').textContent=t};

var loadFiles=function(){
 fetch('/list').then(function(r){return r.json()}).then(function(d){
  var used=d.used,total=d.total;
  var pct=total>0?Math.round(used/total*100):0;
  var mb=function(n){return(n/1048576).toFixed(1)+'MB'};
  var h='<div style="font-size:.8em;color:#8b949e;margin-bottom:6px">'+d.store+': '+mb(used)+' / '+mb(total)+' ('+pct+'%)</div>';
  h+='<table style="width:100%;font-size:.82em"><tr style="color:#8b949e"><th style="text-align:left">File</th><th></th></tr>';
  d.files.forEach(function(f){
   var name=f.split('/').pop();
   h+='<tr><td style="padding:2px 0">'+name+'</td>';
   h+='<td style="text-align:right"><button class="alt" style="padding:2px 8px;font-size:.8em" onclick="delFile(\''+f+'\')">Del</button></td></tr>';
  });
  h+='</table>';
  document.getElementById('files').innerHTML=h||'No files';
 });
};
var delFile=function(f){
 if(!confirm('Delete '+f+'?'))return;
 fetch('/del?f='+encodeURIComponent(f)).then(function(r){return r.text()}).then(function(t){M(t);loadFiles();loadLib()});
};

var loadLib=function(){
 fetch('/list').then(function(r){return r.json()}).then(function(d){
  var h='';
  d.files.forEach(function(f,i){h+='<a onclick="fetch(\'/play?i='+i+'\').then(function(r){return r.text()}).then(M)">'+f+'</a>'});
  document.getElementById('lib').innerHTML=h||'No animations stored';
  M('Storage: '+d.used+'/'+d.total+' bytes ('+d.store+')');
 });
};

var sendGif=function(){
 var f=document.getElementById('gf').files[0];
 if(!f){M('Select a GIF');return}
 var name=document.getElementById('gn').value||f.name.replace(/\.[^.]+$/,'');
 var fd=new FormData();fd.append('file',f);fd.append('name',name);
 var x=new XMLHttpRequest();
 x.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);var b=document.getElementById('gb');b.style.width=p+'%';b.textContent=p+'%'}};
 x.onload=function(){M(x.responseText);loadLib()};
 x.onerror=function(){M('Upload failed')};
 M('Uploading...');x.open('POST','/up/gif');x.send(fd);
};

var findSprites=function(id,w,h,minSz){
 var visited=new Uint8Array(w*h);
 var boxes=[];
 for(var y=0;y<h;y++){for(var x=0;x<w;x++){
  var idx=y*w+x;
  if(visited[idx]||id.data[idx*4+3]<16)continue;
  var x0=x,y0=y,x1=x,y1=y;
  var stack=[idx];
  while(stack.length>0){
   var ci=stack.pop();
   if(visited[ci])continue;
   visited[ci]=1;
   var cx=ci%w,cy=Math.floor(ci/w);
   if(cx<x0)x0=cx;if(cx>x1)x1=cx;if(cy<y0)y0=cy;if(cy>y1)y1=cy;
   var dirs=[-1,1,-w,w];
   for(var d=0;d<4;d++){
    var ni=ci+dirs[d];
    if(ni<0||ni>=w*h||visited[ni])continue;
    if(dirs[d]===-1&&cx===0)continue;
    if(dirs[d]===1&&cx===w-1)continue;
    if(id.data[ni*4+3]>=16)stack.push(ni);
   }
  }
  var bw=x1-x0+1,bh=y1-y0+1;
  if(bw>=minSz&&bh>=minSz)boxes.push({x:x0,y:y0,w:bw,h:bh});
 }}
 boxes.sort(function(a,b){return a.y===b.y?a.x-b.x:a.y-b.y});
 return boxes;
};

var sendSprite=function(){
 var f=document.getElementById('sf').files[0];
 if(!f){M('Select an image');return}
 var name=document.getElementById('sn').value||f.name.replace(/\.[^.]+$/,'');
 var dly=parseInt(document.getElementById('sd').value)||100;
 var minSz=parseInt(document.getElementById('smin').value)||16;
 M('Loading image...');
 var reader=new FileReader();
 reader.onload=function(ev){
  var img=new Image();
  img.onload=function(){
   var cv=document.getElementById('cv');cv.width=img.width;cv.height=img.height;
   var ctx=cv.getContext('2d',{willReadFrequently:true});
   ctx.clearRect(0,0,img.width,img.height);
   ctx.drawImage(img,0,0);
   var id=ctx.getImageData(0,0,img.width,img.height);
   var hasAlpha=false;
   for(var i=3;i<id.data.length;i+=4){if(id.data[i]<240){hasAlpha=true;break;}}
   if(!hasAlpha){
    var cr=id.data[0],cg=id.data[1],cb=id.data[2],tol=30;
    for(var i=0;i<id.data.length;i+=4){
     var dr=Math.abs(id.data[i]-cr),dg=Math.abs(id.data[i+1]-cg),db=Math.abs(id.data[i+2]-cb);
     if(dr<tol&&dg<tol&&db<tol)id.data[i+3]=0;
    }
   }
   M('Detecting sprites...');
   var boxes=findSprites(id,img.width,img.height,minSz);
   if(boxes.length===0)boxes=[{x:0,y:0,w:img.width,h:img.height}];
   var rows=[],curRow=[boxes[0]];
   for(var i=1;i<boxes.length;i++){
    var b=boxes[i],prev=curRow[0];
    if(b.y>=prev.y+prev.h+minSz){rows.push(curRow);curRow=[b];}
    else curRow.push(b);
   }
   rows.push(curRow);
   M('Found '+boxes.length+' sprites in '+rows.length+' rows');
   var uploadRow=function(ri){
    if(ri>=rows.length){M('Done! '+rows.length+' animations uploaded');loadLib();return;}
    var row=rows[ri];
    var nf=row.length;if(nf>MAX_FRAMES)nf=MAX_FRAMES;
    var rawW=0,rawH=0;
    for(var i=0;i<nf;i++){if(row[i].w>rawW)rawW=row[i].w;if(row[i].h>rawH)rawH=row[i].h;}
    var scale=1.0;
    if(rawW>478||rawH>478)scale=Math.min(478/rawW,478/rawH);
    var maxW=Math.round(rawW*scale),maxH=Math.round(rawH*scale);
    if(maxW>478)maxW=478;if(maxH>478)maxH=478;
    if(maxW<1)maxW=1;if(maxH<1)maxH=1;
    var bpf=maxW*maxH*3,maxUp=4*1024*1024;
    if(nf*bpf>maxUp){nf=Math.floor(maxUp/bpf);if(nf<1)nf=1;}
    var total=nf*bpf;
    var out=new Uint8Array(total),off=0;
    var cv2=document.createElement('canvas');cv2.width=maxW;cv2.height=maxH;
    var ctx2=cv2.getContext('2d',{willReadFrequently:true});
    ctx2.imageSmoothingEnabled=false;
    for(var i=0;i<nf;i++){
     var b=row[i];
     ctx2.clearRect(0,0,maxW,maxH);
     var sw=Math.round(b.w*scale),sh=Math.round(b.h*scale);
     var dx=Math.floor((maxW-sw)/2),dy=Math.floor((maxH-sh)/2);
     ctx2.drawImage(img,b.x,b.y,b.w,b.h,dx,dy,sw,sh);
     var fd2=ctx2.getImageData(0,0,maxW,maxH);
     var px=fd2.data;
     for(var j=0;j<px.length;j+=4){
      var v=((px[j]>>3)<<11)|((px[j+1]>>2)<<5)|(px[j+2]>>3);
      out[off++]=v&0xFF;out[off++]=(v>>8)&0xFF;out[off++]=px[j+3];
     }
    }
    var rn=rows.length>1?name+'_r'+ri:name;
    M('Uploading row '+(ri+1)+'/'+rows.length+': '+nf+' frames '+maxW+'x'+maxH);
    var blob=new Blob([out.subarray(0,off)],{type:'application/octet-stream'});
    var fd=new FormData();fd.append('name',rn);
    fd.append('fw',''+maxW);fd.append('fh',''+maxH);
    fd.append('nf',''+nf);fd.append('dl',''+dly);
    fd.append('file',blob,'s.bin');
    var x=new XMLHttpRequest();
    x.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);var b=document.getElementById('sb');b.style.width=p+'%';b.textContent=p+'%'}};
    x.onload=function(){M('Row '+(ri+1)+': '+x.responseText);uploadRow(ri+1);};
    x.onerror=function(){M('Row '+(ri+1)+' failed');uploadRow(ri+1);};
    x.open('POST','/up/sprite');x.send(fd);
   };
   uploadRow(0);
  };
  img.src=ev.target.result;
 };
 reader.readAsDataURL(f);
};
var liveTimer=null;
var grabScreen=function(){
 var el=document.getElementById('screenImg');
 el.src='/screenshot?t='+Date.now();
 el.style.display='';
 M('Screenshot captured');
};
var toggleLive=function(){
 var btn=document.getElementById('liveBtn');
 if(liveTimer){clearInterval(liveTimer);liveTimer=null;btn.textContent='Live View';M('Live view stopped');return}
 btn.textContent='Stop Live';
 var el=document.getElementById('screenImg');el.style.display='';
 var refresh=function(){el.src='/screenshot?t='+Date.now()};
 refresh();
 liveTimer=setInterval(refresh,2000);
 M('Live view (2s refresh)');
};
window.onload=function(){loadLib();loadFiles()};
</script></body></html>
)rawhtml";