#!/usr/bin/env python3
"""Scripted local JSONL peer. No model, network, account or generated-art claim."""
import json, os, sys

thread='synthetic-thread';turn='synthetic-turn';task=1;revision=0
stage='';counter=100;last_edit=None
question=os.environ.get('PIXELFORGE_TEST_QUESTION')=='1'
hold=os.environ.get('PIXELFORGE_TEST_HOLD')=='1'
def write(value):print(json.dumps(value,separators=(',',':')),flush=True)
def tool(name,args,call=None):
    global counter,stage
    counter+=1;stage=name+':'+args.get('action','')
    p={'threadId':thread,'turnId':turn,'callId':call or 'call-'+str(counter),'namespace':None,'tool':name,'arguments':args}
    write({'id':counter,'method':'item/tool/call','params':p});return p
def bound(**more):return dict(task_id=task,expected_revision=revision,**more)
for line in sys.stdin:
    m=json.loads(line);method=m.get('method');p=m.get('params',{})
    if method=='initialize':
        assert p['capabilities']['experimentalApi']
        write({'id':m['id'],'result':{}})
    elif method=='initialized':pass
    elif method=='account/read':write({'id':m['id'],'result':{'account':{'type':'chatgpt'}}})
    elif method=='model/list':write({'id':m['id'],'result':{'data':[{'id':'gpt-6-astra','model':'gpt-6-astra','supportedReasoningEfforts':[{'reasoningEffort':'medium'}]}],'nextCursor':None}})
    elif method=='thread/start':
        assert p['approvalPolicy']=='never' and p['sandbox']=='read-only' and p['environments']==[]
        assert 'pixelforge_program' in [t['name'] for t in p['dynamicTools']]
        write({'id':m['id'],'result':{'thread':{'id':thread},'model':p['model'],'reasoningEffort':p['config']['model_reasoning_effort'],'cwd':p['cwd'],'sandbox':{'type':'readOnly'},'approvalPolicy':'never'}})
    elif method=='turn/start':
        assert p['model']=='gpt-6-astra' and p['effort']=='medium'
        write({'id':m['id'],'result':{'turn':{'id':turn,'status':'inProgress'}}})
        write({'method':'turn/started','params':{'threadId':thread,'turn':{'id':turn}}})
        tool('pixelforge_task',{'action':'get'})
    elif method=='turn/interrupt':
        write({'id':m['id'],'result':{}})
        stage='late'
        write({'id':999,'method':'item/tool/call','params':{'threadId':thread,'turnId':turn,'callId':'late-after-stop','namespace':None,'tool':'pixelforge_edit','arguments':bound(patch='P,0,0,#FFFFFFFF')}})
    elif 'result' in m and int(m['id'])>=100:
        r=m['result'];facts=json.loads(r['contentItems'][0]['text']) if stage!='late' else {}
        if stage=='late':
            assert not r['success']
            write({'method':'turn/completed','params':{'threadId':thread,'turn':{'id':turn,'status':'interrupted'}}});break
        if stage=='pixelforge_task:get':
            task=facts['task_id'];revision=facts['revision']
            if question:tool('pixelforge_dialog',{'task_id':task,'reason':'Explicit test question','question':'Background preference?','suggestions':'Transparent|Solid'})
            else:tool('pixelforge_task',{'action':'accept','task_id':task,'width':16,'height':16})
        elif stage=='pixelforge_dialog:':
            assert r['success'] and facts['answer']=='Keep a transparent background'
            tool('pixelforge_task',{'action':'accept','task_id':task,'width':16,'height':16})
        elif stage=='pixelforge_task:accept':
            assert r['success'];revision=facts['revision']
            tool('pixelforge_edit',dict(task_id=task,expected_revision=revision-1,patch='P,0,0,#FFFFFFFF'));stage='stale'
        elif stage=='stale':
            assert not r['success'];tool('pixelforge_program',bound(program='R 2 2 5 4 #FFEB546C\nL 2 8 12 8 #FF32B8B1'))
        elif stage=='pixelforge_program:':
            assert r['success'];revision=facts['revision']
            if hold:
                write({'method':'item/completed','params':{'threadId':thread,'item':{'type':'agentMessage','text':'Scripted fixture waiting for Stop after real controller edit.'}}})
                stage='holding'
            else:tool('pixelforge_view',bound(action='render',scale=4))
        elif stage=='pixelforge_view:render':
            assert r['success'] and r['contentItems'][1]['imageUrl'].startswith('data:image/png;base64,')
            tool('pixelforge_task',bound(action='finish',summary='Synthetic transport fixture artwork ready for human review.'))
        elif stage=='pixelforge_task:finish':
            assert r['success'] and facts['awaiting_user_review']
            write({'method':'turn/completed','params':{'threadId':thread,'turn':{'id':turn,'status':'completed'}}});break
