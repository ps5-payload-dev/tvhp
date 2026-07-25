async function main() {
    const CWD = window.workingDir;

    return {
        mainText: "TVHP",
	secondaryText: 'A TVHeadend player',
	onclick: async () => {
	    return {
		path: CWD + '/tvhp',
		cwd: CWD
	    };
        }
    };
}
