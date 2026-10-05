export function LikeButton(props, { rsc, useState }) {
    const [liked, setLiked] = useState(false);
    const [pending, setPending] = useState(false);

    const serverCount = (props && props.count != null) ? props.count : 0;
    const count = serverCount + (liked ? 1 : 0);

    const el = document.createElement('span');
    el.className = 'like-btn';

    const btn = document.createElement('button');
    btn.textContent = (liked ? '\u2764 ' : '\u2661 ') + count;
    btn.title = props && props.id ? ('like: ' + props.id) : 'like';

    btn.addEventListener('click', () => {
        if (pending) return;
        setPending(true);
        setLiked((prev) => !prev);
        rsc.action('like:toggle', { id: props.id, liked: !liked }).finally(() => setPending(false));
    });

    const tag = document.createElement('span');
    tag.className = 'tag';
    tag.textContent = 'client';

    el.appendChild(btn);
    el.appendChild(tag);
    return el;
}
