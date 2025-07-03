import pathlib
import os

# vim_output_file = pathlib.Path(__file__).parent / 'vim_output.txt'
# vim_keystrokes_file = pathlib.Path(__file__).parent / 'vim_keystrokes.txt'
test_cases_folder = pathlib.Path(__file__).parent / 'test_cases'

def get_current_test_case_index():
    if not test_cases_folder.exists():
        return 0
    test_cases = list(test_cases_folder.glob('test_case_*.txt'))
    if not test_cases:
        return 0
    # Sort by numerical index instead of alphabetically
    test_cases.sort(key=lambda x: int(x.name.split('_')[2].split('.')[0]))
    last_test_case = test_cases[-1]
    index = int(last_test_case.name.split('_')[2].split('.')[0])
    return index + 1

if __name__ == '__main__':
    while True:
        inp =  input("Enter to continue, 'q' to quit: ")
        if inp.lower() == 'q':
            break
        current_test_case_index = get_current_test_case_index()
        vim_output_file = test_cases_folder / f'test_case_{current_test_case_index}.txt'
        vim_keystrokes_file = test_cases_folder / f'test_case_{current_test_case_index}.keystrokes.txt'
        # launch vim with -W option to write the output to the file
        os.system(f'vim {vim_output_file} -W {vim_keystrokes_file}')

