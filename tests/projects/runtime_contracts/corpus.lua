-- Sources and independently stated outcomes, never pre-recorded runtime traces.
return {
    {name='language', entry='language.ks', scenes={'language.ks'},
     variables={number=3, short=false, pick='甲'}, texts=4, pages=1},
    {name='macros', entry='macros.ks', scenes={'macros.ks'},
     variables={count=3}, texts=3},
    {name='calls', entry='calls.ks', scenes={'calls.ks','callee.ks'},
     variables={total=3, returned='caller', nested='callee'}, texts=5},
    {name='choices', entry='choices.ks', scenes={'choices.ks'},
     variables={enabled=true, route='right'}, texts=2, menus=1, choice=2},
    {name='timing', entry='timing.ks', scenes={'timing.ks'},
     variables={finished=1}, texts=2, message_x=100},
    {name='restore', entry='restore.ks', scenes={'restore.ks','saved_callee.ks','loader.ks'},
     variables={reward=1, returned='caller'}, texts=3, replay='loader.ks', replay_texts=2},
}
